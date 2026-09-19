#include "FloppyDiskManager.hpp"
#include "AppPaths.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "Connector/CE1600FCard.hpp"
#include "PC1600/PC1600Machine.hpp"

namespace {

constexpr auto kFloppySuffix = ".floppy.img";

QVector<FloppyDiskManager::DiskEntry> entriesFor(const QString& dir) {
    QVector<FloppyDiskManager::DiskEntry> out;
    QDir d(dir);
    for (const QFileInfo& fi : d.entryInfoList({QLatin1Char('*') + QLatin1String(kFloppySuffix)}, QDir::Files, QDir::Name)) {
        QString name = fi.fileName();
        name.chop(static_cast<int>(qstrlen(kFloppySuffix)));
        out.push_back({name});
    }
    return out;
}

// Finds `<diskName>.floppy.img` in the instance dir first, then the
// bundled dir (an instance shadows a bundled template of the same name --
// same precedence MemoryModuleManager gives instance cards).
bool resolveDiskPath(const QString& diskName, QString* outPath, bool* outIsInstance) {
    const QString instPath = AppPaths::floppyInstancePathFor(diskName);
    if (QFile::exists(instPath)) {
        *outPath = instPath;
        *outIsInstance = true;
        return true;
    }
    const QString bundledPath =
        QDir(AppPaths::bundledResourcesDir()).filePath(AppPaths::sanitizedFloppyFileName(diskName));
    if (QFile::exists(bundledPath)) {
        *outPath = bundledPath;
        *outIsInstance = false;
        return true;
    }
    return false;
}

}  // namespace

FloppyDiskManager::FloppyDiskManager(MachineController* controller, QObject* parent)
    : QObject(parent), m_controller(controller) {
    m_debounceTimer = new QTimer(this);
    m_debounceTimer->setSingleShot(true);
    connect(m_debounceTimer, &QTimer::timeout, this, &FloppyDiskManager::flushPendingPersist);
}

QVector<FloppyDiskManager::DiskEntry> FloppyDiskManager::bundledEntries() const {
    return entriesFor(AppPaths::bundledResourcesDir());
}

QVector<FloppyDiskManager::DiskEntry> FloppyDiskManager::instanceEntries() const {
    return entriesFor(AppPaths::instanceDir());
}

void FloppyDiskManager::selectDisk(const QString& diskNameOrEmpty) {
    flushPendingPersist();
    m_diskName = diskNameOrEmpty;
    m_instanceFilePath.clear();
    m_persistPending = false;

    auto* m1600 = m_controller->pc1600();
    if (!m1600 || !m1600->ce1600fAttached()) return;

    if (!loadSelectedDisk(m1600)) {
        m_diskName.clear();
        m1600->ce1600fInsertBlank();
    }
    m_lastSeenRevision = m1600->ce1600fRevision();
}

bool FloppyDiskManager::loadSelectedDisk(PC1600Machine* m1600) {
    if (m_diskName.isEmpty()) return false;  // blank disk requested

    QString path;
    bool isInstance = false;
    if (!resolveDiskPath(m_diskName, &path, &isInstance)) {
        emit errorMessage(tr("Couldn't find the floppy disk \"%1\".").arg(m_diskName));
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        emit errorMessage(tr("Couldn't read \"%1\".").arg(path));
        return false;
    }
    const QByteArray bytes = f.readAll();
    const auto data = reinterpret_cast<const uint8_t*>(bytes.constData());
    if (!m1600->ce1600fLoadImage(data, static_cast<size_t>(bytes.size()))) {
        emit errorMessage(tr("\"%1\" isn't a valid CE-1600F floppy image (expected %2 bytes).")
                               .arg(m_diskName)
                               .arg(CE1600FCard::kImageSize));
        return false;
    }
    if (isInstance) m_instanceFilePath = path;
    return true;
}

void FloppyDiskManager::attachToMachine() {
    auto* m1600 = m_controller->pc1600();
    if (!m1600 || !m1600->ce1600fAttached()) return;
    if (m_diskName.isEmpty()) {
        // attachCE1600P() already inserted a blank disk by default.
        m_lastSeenRevision = m1600->ce1600fRevision();
        return;
    }
    const QString name = m_diskName;
    selectDisk(name);  // re-resolves and loads it into the freshly attached card
}

void FloppyDiskManager::syncFromPresetLoad(const QString& labelOrEmpty, const QString& resolvedPathOrEmpty) {
    m_diskName = labelOrEmpty;
    m_instanceFilePath = AppPaths::isUnderDir(resolvedPathOrEmpty, AppPaths::instanceDir()) ? resolvedPathOrEmpty : QString();
    m_persistPending = false;
    if (auto* m1600 = m_controller->pc1600()) m_lastSeenRevision = m1600->ce1600fRevision();
}

bool FloppyDiskManager::nameCollides(const QString& diskName) const {
    return QFile::exists(AppPaths::floppyInstancePathFor(diskName));
}

bool FloppyDiskManager::nameAndSave(const QString& diskName, QString* error) {
    const QString name = diskName.trimmed();
    if (name.isEmpty()) {
        *error = tr("Name cannot be empty.");
        return false;
    }
    auto* m1600 = m_controller->pc1600();
    if (!m1600 || !m1600->ce1600fAttached()) {
        *error = tr("No floppy attached.");
        return false;
    }
    if (name != m_diskName && nameCollides(name)) {
        *error = tr("A disk named \"%1\" already exists. Choose a different name.").arg(name);
        return false;
    }

    const auto image = m1600->ce1600fDiskImage();
    const QString newPath = AppPaths::floppyInstancePathFor(name);
    if (!AppPaths::atomicWriteBinaryFile(newPath, image)) {
        *error = tr("Couldn't write \"%1\".").arg(newPath);
        return false;
    }

    m_diskName = name;
    m_instanceFilePath = newPath;
    m_persistPending = false;
    m_lastSeenRevision = m1600->ce1600fRevision();
    return true;
}

void FloppyDiskManager::writeInstance() {
    if (m_instanceFilePath.isEmpty()) return;
    auto* m1600 = m_controller->pc1600();
    if (!m1600 || !m1600->ce1600fAttached()) return;
    const auto image = m1600->ce1600fDiskImage();
    AppPaths::atomicWriteBinaryFile(m_instanceFilePath, image);
}

void FloppyDiskManager::markDirtyAndSchedulePersist() {
    auto* m1600 = m_controller->pc1600();
    if (!m1600 || !m1600->ce1600fAttached()) return;
    const uint64_t rev = m1600->ce1600fRevision();
    if (rev == m_lastSeenRevision) return;
    m_lastSeenRevision = rev;
    if (m_instanceFilePath.isEmpty()) return;  // dirty, but nothing to autosave to yet
    m_persistPending = true;
    if (m_debounceTimer->isActive()) return;  // already pending -- coalesce
    m_debounceTimer->start(500);
}

int FloppyDiskManager::side() const {
    auto* m1600 = m_controller->pc1600();
    return m1600 ? m1600->ce1600fSide() : 0;
}

void FloppyDiskManager::toggleSide() {
    auto* m1600 = m_controller->pc1600();
    if (!m1600 || !m1600->ce1600fAttached()) return;
    m1600->ce1600fSetSide(m1600->ce1600fSide() == 0 ? 1 : 0);
    m_lastSeenRevision = m1600->ce1600fRevision();
}

bool FloppyDiskManager::motorOn() const {
    auto* m1600 = m_controller->pc1600();
    return m1600 && m1600->ce1600fMotorOn();
}

void FloppyDiskManager::flushPendingPersist() {
    m_debounceTimer->stop();
    if (!m_persistPending) return;
    writeInstance();
    m_persistPending = false;
}
