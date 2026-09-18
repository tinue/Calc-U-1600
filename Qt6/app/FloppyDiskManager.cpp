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
    for (const QFileInfo& fi : d.entryInfoList({QStringLiteral("*.floppy.img")}, QDir::Files, QDir::Name)) {
        QString name = fi.fileName();
        name.chop(static_cast<int>(qstrlen(kFloppySuffix)));
        out.push_back({name});
    }
    return out;
}

bool pathIsUnderDir(const QString& path, const QString& dir) {
    if (path.isEmpty() || dir.isEmpty()) return false;
    const QString a = QFileInfo(path).canonicalFilePath();
    const QString b = QFileInfo(dir).canonicalFilePath();
    return !a.isEmpty() && !b.isEmpty() && a.startsWith(b);
}

// Finds `<diskName>.floppy.img` in the instance dir first, then the
// bundled dir (an instance shadows a bundled template of the same name --
// same precedence MemoryModuleManager gives instance cards).
bool resolveDiskPath(const QString& diskName, QString* outPath, bool* outIsInstance) {
    const QString fileName = FloppyDiskManager::DiskEntry{diskName}.diskName + QString::fromLatin1(kFloppySuffix);
    const QString instPath = QDir(AppPaths::instanceDir()).filePath(fileName);
    if (QFile::exists(instPath)) {
        *outPath = instPath;
        *outIsInstance = true;
        return true;
    }
    const QString bundledPath = QDir(AppPaths::bundledResourcesDir()).filePath(fileName);
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

    if (m_diskName.isEmpty()) {
        m1600->ce1600fInsertBlank();
        m_lastSeenRevision = m1600->ce1600fRevision();
        return;
    }

    QString path;
    bool isInstance = false;
    if (!resolveDiskPath(m_diskName, &path, &isInstance)) {
        emit errorMessage(tr("Couldn't find the floppy disk \"%1\".").arg(m_diskName));
        m_diskName.clear();
        m1600->ce1600fInsertBlank();
        m_lastSeenRevision = m1600->ce1600fRevision();
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        emit errorMessage(tr("Couldn't read \"%1\".").arg(path));
        m_diskName.clear();
        m1600->ce1600fInsertBlank();
        m_lastSeenRevision = m1600->ce1600fRevision();
        return;
    }
    const QByteArray bytes = f.readAll();
    const auto data = reinterpret_cast<const uint8_t*>(bytes.constData());
    if (!m1600->ce1600fLoadImage(data, static_cast<size_t>(bytes.size()))) {
        emit errorMessage(tr("\"%1\" isn't a valid CE-1600F floppy image (expected %2 bytes).")
                               .arg(m_diskName)
                               .arg(CE1600FCard::kImageSize));
        m_diskName.clear();
        m1600->ce1600fInsertBlank();
    } else if (isInstance) {
        m_instanceFilePath = path;
    }
    m_lastSeenRevision = m1600->ce1600fRevision();
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
    m_instanceFilePath = pathIsUnderDir(resolvedPathOrEmpty, AppPaths::instanceDir()) ? resolvedPathOrEmpty : QString();
    m_persistPending = false;
    if (auto* m1600 = m_controller->pc1600()) m_lastSeenRevision = m1600->ce1600fRevision();
    emit diskChanged();
}

bool FloppyDiskManager::nameCollides(const QString& diskName) const {
    const QString instPath = QDir(AppPaths::instanceDir()).filePath(diskName + QString::fromLatin1(kFloppySuffix));
    return QFile::exists(instPath);
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
    const QString newPath = QDir(AppPaths::instanceDir()).filePath(name + QString::fromLatin1(kFloppySuffix));
    if (!AppPaths::atomicWriteBinaryFile(newPath, image)) {
        *error = tr("Couldn't write \"%1\".").arg(newPath);
        return false;
    }

    m_diskName = name;
    m_instanceFilePath = newPath;
    m_persistPending = false;
    m1600->ce1600fClearDirty();
    m_lastSeenRevision = m1600->ce1600fRevision();
    emit diskChanged();
    return true;
}

void FloppyDiskManager::writeInstance() {
    if (m_instanceFilePath.isEmpty()) return;
    auto* m1600 = m_controller->pc1600();
    if (!m1600 || !m1600->ce1600fAttached()) return;
    const auto image = m1600->ce1600fDiskImage();
    if (AppPaths::atomicWriteBinaryFile(m_instanceFilePath, image)) m1600->ce1600fClearDirty();
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

void FloppyDiskManager::flushPendingPersist() {
    m_debounceTimer->stop();
    if (!m_persistPending) return;
    writeInstance();
    m_persistPending = false;
}
