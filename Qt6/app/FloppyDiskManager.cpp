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

FloppyDiskManager::DiskLists FloppyDiskManager::diskLists() const {
    DiskLists lists;
    for (const auto& e : scanFloppyDirectory(AppPaths::bundledResourcesDir().toStdString(), nullptr))
        lists.templates.push_back({QString::fromStdString(e.diskName)});
    const QVector<DiskEntry> bundled = lists.templates;
    for (const auto& e : scanFloppyDirectory(AppPaths::instanceDir().toStdString(), nullptr)) {
        const QString name = QString::fromStdString(e.diskName);
        if (containsName(bundled, name)) continue;
        (e.isTemplate ? lists.templates : lists.instances).push_back({name});
    }
    return lists;
}

QVector<FloppyDiskManager::DiskEntry> FloppyDiskManager::templateEntries() const {
    return diskLists().templates;
}

void FloppyDiskManager::classifySource(const QString& resolvedPathOrEmpty, bool isTemplate) {
    m_isTemplate = isTemplate;
    m_instanceFilePath.clear();
    if (!resolvedPathOrEmpty.isEmpty() && !isTemplate &&
        !AppPaths::isUnderDir(resolvedPathOrEmpty, AppPaths::bundledResourcesDir()))
        m_instanceFilePath = resolvedPathOrEmpty;
}

void FloppyDiskManager::selectDisk(const QString& diskNameOrEmpty) {
    flushPendingPersist();
    m_diskName = diskNameOrEmpty;
    classifySource(QString(), false);
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
    classifySource(QString::fromStdString(path), disk.isTemplate);
    return true;
}

void FloppyDiskManager::insertSelectedDisk() {
    selectDisk(QString(m_diskName));  // copy: selectDisk() reassigns m_diskName
}

void FloppyDiskManager::syncFromPresetLoad(const QString& labelOrEmpty, const QString& resolvedPathOrEmpty) {
    m_diskName = labelOrEmpty;
    FloppyCatalogEntry entry;
    const bool isTemplate = !resolvedPathOrEmpty.isEmpty() &&
                            readFloppyCatalogEntry(resolvedPathOrEmpty.toStdString(), &entry, nullptr) &&
                            entry.isTemplate;
    classifySource(labelOrEmpty.isEmpty() ? QString() : resolvedPathOrEmpty, isTemplate);
    m_persistPending = false;
    if (auto* m1600 = m_controller->pc1600()) m_lastSeenRevision = m1600->ce1600fRevision();
}

bool FloppyDiskManager::nameCollides(const QString& diskName) const {
    return containsName(entriesFor(AppPaths::instanceDir()), diskName) ||
           QFile::exists(AppPaths::floppyInstancePathFor(diskName));
}

bool FloppyDiskManager::nameAndSave(const QString& diskName, QString* error) {
    return saveDiskAs(diskName, /*fromPreset=*/false, error);
}

bool FloppyDiskManager::saveAsFromPreset(const QString& diskName, QString* error) {
    return saveDiskAs(diskName, /*fromPreset=*/true, error);
}

bool FloppyDiskManager::saveDiskAs(const QString& diskName, bool fromPreset, QString* error) {
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
    if (!fromPreset && !m_isTemplate) {
        *error = tr("\"%1\" is already saved; changes are saved automatically.").arg(m_diskName);
        return false;
    }
    if (name.contains(QLatin1Char('"'))) {
        *error = tr("Name cannot contain '\"'.");
        return false;
    }
    if (containsName(templateEntries(), name)) {
        *error = tr("\"%1\" is a template's name. Choose a different name.").arg(name);
        return false;
    }
    if (!fromPreset && nameCollides(name)) {
        *error = tr("A disk named \"%1\" already exists. Choose a different name.").arg(name);
        return false;
    }

    const QString newPath = AppPaths::floppyInstancePathFor(name);
    FloppyCatalogEntry existing;
    if (readFloppyCatalogEntry(newPath.toStdString(), &existing, nullptr) && existing.isTemplate) {
        *error = tr("\"%1\" is a template file and is never overwritten. Choose a different name.").arg(newPath);
        return false;
    }
    if (!AppPaths::atomicWriteFile(newPath, formatFloppyFile(name.toStdString(), m1600->ce1600fDiskImage()))) {
        *error = tr("Couldn't write \"%1\".").arg(newPath);
        return false;
    }

    // Retarget the drive at the saved copy: it now shows under its new name
    // and autosaves there.
    m_diskName = name;
    classifySource(newPath, /*isTemplate=*/false);  // formatFloppyFile() never writes `template:`
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
    return m1600 && m1600->ce1600fHasDisk() && m_isTemplate;
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
