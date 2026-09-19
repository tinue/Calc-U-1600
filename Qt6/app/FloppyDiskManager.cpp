#include "FloppyDiskManager.hpp"
#include "AppPaths.hpp"

#include <QFile>

#include <algorithm>

#include "Connector/CE1600FCard.hpp"
#include "Connector/FloppyImageFile.hpp"
#include "PC1600/PC1600Machine.hpp"

namespace {

QVector<FloppyDiskManager::DiskEntry> entriesFor(const QString& dir) {
    QVector<FloppyDiskManager::DiskEntry> out;
    for (const auto& e : scanFloppyDirectory(dir.toStdString(), nullptr))
        out.push_back({QString::fromStdString(e.diskName)});
    return out;
}

bool containsName(const QVector<FloppyDiskManager::DiskEntry>& entries, const QString& diskName) {
    for (const auto& e : entries)
        if (e.diskName == diskName) return true;
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

// Leaves out saved disks that share a bundled disk's name: lookup is
// bundled-first, so they could never be loaded.
QVector<FloppyDiskManager::DiskEntry> FloppyDiskManager::instanceEntries() const {
    QVector<DiskEntry> out = entriesFor(AppPaths::instanceDir());
    const QVector<DiskEntry> bundled = bundledEntries();
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const DiskEntry& e) { return containsName(bundled, e.diskName); }),
              out.end());
    return out;
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
        m1600->ce1600fEject();
    }
    m_lastSeenRevision = m1600->ce1600fRevision();
}

bool FloppyDiskManager::loadSelectedDisk(PC1600Machine* m1600) {
    if (m_diskName.isEmpty()) return false;  // "–empty–": no disk

    // Bundled first, then the save folder -- the same order the preset
    // loader and MemoryModuleManager use.
    const std::vector<std::string> dirs = {AppPaths::bundledResourcesDir().toStdString(),
                                           AppPaths::instanceDir().toStdString()};
    std::string path, err;
    FloppyFile disk;
    if (!resolveFloppyByName(dirs, m_diskName.toStdString(), &path, &err) || !readFloppyFile(path, &disk, &err)) {
        emit errorMessage(tr("Couldn't load the floppy disk \"%1\": %2").arg(m_diskName, QString::fromStdString(err)));
        return false;
    }
    m1600->ce1600fLoadImage(disk.image.data(), disk.image.size());
    const QString resolved = QString::fromStdString(path);
    if (AppPaths::isUnderDir(resolved, AppPaths::instanceDir())) m_instanceFilePath = resolved;
    return true;
}

void FloppyDiskManager::insertSelectedDisk() {
    selectDisk(QString(m_diskName));  // copy: selectDisk() reassigns m_diskName
}

void FloppyDiskManager::syncFromPresetLoad(const QString& labelOrEmpty, const QString& resolvedPathOrEmpty) {
    m_diskName = labelOrEmpty;
    m_instanceFilePath = AppPaths::isUnderDir(resolvedPathOrEmpty, AppPaths::instanceDir()) ? resolvedPathOrEmpty : QString();
    m_persistPending = false;
    if (auto* m1600 = m_controller->pc1600()) m_lastSeenRevision = m1600->ce1600fRevision();
}

bool FloppyDiskManager::nameCollides(const QString& diskName) const {
    return containsName(entriesFor(AppPaths::instanceDir()), diskName) ||
           QFile::exists(AppPaths::floppyInstancePathFor(diskName));
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
    if (!m1600->ce1600fHasDisk()) {
        *error = tr("There's no disk in the drive.");
        return false;
    }
    if (hasInstanceFile()) {
        *error = tr("\"%1\" is already saved; changes are saved automatically.").arg(m_diskName);
        return false;
    }
    if (name.contains(QLatin1Char('"'))) {
        *error = tr("Name cannot contain '\"'.");
        return false;
    }
    if (containsName(bundledEntries(), name)) {
        *error = tr("\"%1\" is a built-in disk name. Choose a different name.").arg(name);
        return false;
    }
    if (nameCollides(name)) {
        *error = tr("A disk named \"%1\" already exists. Choose a different name.").arg(name);
        return false;
    }

    const QString newPath = AppPaths::floppyInstancePathFor(name);
    if (!AppPaths::atomicWriteFile(newPath, formatFloppyFile(name.toStdString(), m1600->ce1600fDiskImage()))) {
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
    AppPaths::atomicWriteFile(m_instanceFilePath,
                              formatFloppyFile(m_diskName.toStdString(), m1600->ce1600fDiskImage()));
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

bool FloppyDiskManager::canNameAndSave() const {
    auto* m1600 = m_controller->pc1600();
    return m1600 && m1600->ce1600fHasDisk() && !hasInstanceFile();
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
