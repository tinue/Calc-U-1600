#include "Shared/FloppyBay.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "Connector/CE1600FCard.hpp"
#include "Connector/FloppyImageFile.hpp"
#include "PC1600/PC1600Machine.hpp"
#include "Shared/AppPaths.hpp"
#include "Shared/MachineSession.hpp"
#include "Shared/Utf8Path.hpp"

namespace shared {

namespace {

std::vector<std::string> namesIn(const std::string& dir) {
    std::vector<std::string> out;
    for (const auto& e : scanFloppyDirectory(dir, nullptr)) out.push_back(e.diskName);
    return out;
}

bool contains(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

std::string trimmed(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string inQuotes(const std::string& s) { return "\"" + s + "\""; }

} // namespace

FloppyBay::FloppyBay(MachineSession& session, const AppPaths& paths, Events events)
    : m_session(session), m_paths(paths), m_events(std::move(events)) {}

PC1600Machine* FloppyBay::drive() const {
    PC1600Machine* m = m_session.pc1600();
    return m && m->ce1600fAttached() ? m : nullptr;
}

FloppyBay::DiskLists FloppyBay::diskLists() const {
    DiskLists lists;
    lists.templates = namesIn(m_paths.bundledResourcesDir());
    const std::vector<std::string> bundled = lists.templates;
    for (const auto& e : scanFloppyDirectory(m_paths.instanceDir(), nullptr)) {
        if (contains(bundled, e.diskName)) continue;
        (e.isTemplate ? lists.templates : lists.instances).push_back(e.diskName);
    }
    return lists;
}

void FloppyBay::classifySource(const std::string& resolvedPathOrEmpty, bool isTemplate) {
    m_isTemplate = isTemplate;
    m_instanceFilePath.clear();
    if (!resolvedPathOrEmpty.empty() && !isTemplate && !isUnderDir(resolvedPathOrEmpty, m_paths.bundledResourcesDir()))
        m_instanceFilePath = resolvedPathOrEmpty;
}

void FloppyBay::selectDisk(const std::string& diskNameOrEmpty) {
    flushPendingPersist();
    m_diskName = diskNameOrEmpty;
    classifySource({}, false);
    m_persistPending = false;

    PC1600Machine* m1600 = drive();
    if (!m1600) return;
    if (!loadSelectedDisk(m1600)) {
        m_diskName.clear();
        m1600->ce1600fEject();
    }
    m_lastSeenRevision = m1600->ce1600fRevision();
}

bool FloppyBay::loadSelectedDisk(PC1600Machine* m1600) {
    if (m_diskName.empty()) return false;  // "–empty–": no disk
    // Bundled first, then the save folder -- the order the preset loader
    // and the module bay use.
    std::string path, err;
    FloppyFile disk;
    if (!resolveFloppyByName({m_paths.bundledResourcesDir(), m_paths.instanceDir()}, m_diskName, &path, &err) ||
        !readFloppyFile(path, &disk, &err)) {
        reportError("Couldn't load the floppy disk " + inQuotes(m_diskName) + ": " + err);
        return false;
    }
    m1600->ce1600fLoadImage(disk.image.data(), disk.image.size());
    classifySource(path, disk.isTemplate);
    return true;
}

void FloppyBay::insertSelectedDisk() {
    selectDisk(std::string(m_diskName));  // copy: selectDisk() reassigns m_diskName
}

void FloppyBay::syncFromPresetLoad(const std::string& labelOrEmpty, const std::string& resolvedPathOrEmpty) {
    m_diskName = labelOrEmpty;
    FloppyCatalogEntry entry;
    const bool isTemplate = !resolvedPathOrEmpty.empty() &&
                            readFloppyCatalogEntry(resolvedPathOrEmpty, &entry, nullptr) && entry.isTemplate;
    classifySource(labelOrEmpty.empty() ? std::string() : resolvedPathOrEmpty, isTemplate);
    m_persistPending = false;
    if (PC1600Machine* m1600 = m_session.pc1600()) m_lastSeenRevision = m1600->ce1600fRevision();
}

bool FloppyBay::nameCollides(const std::string& diskName) const {
    std::error_code ec;
    return contains(namesIn(m_paths.instanceDir()), diskName) ||
           std::filesystem::exists(toPath(m_paths.floppyInstancePathFor(diskName)), ec);
}

bool FloppyBay::nameAndSave(const std::string& diskName, std::string* error) {
    return saveDiskAs(diskName, /*fromPreset=*/false, error);
}

bool FloppyBay::saveAsFromPreset(const std::string& diskName, const std::string& filePath, bool asTemplate,
                                 std::string* error) {
    return saveDiskAs(diskName, /*fromPreset=*/true, error, filePath, asTemplate);
}

bool FloppyBay::saveDiskAs(const std::string& diskName, bool fromPreset, std::string* error,
                           const std::string& filePath, bool asTemplate) {
    const std::string name = trimmed(diskName);
    if (name.empty()) {
        *error = "Name cannot be empty.";
        return false;
    }
    PC1600Machine* m1600 = drive();
    if (!m1600) {
        *error = "No floppy attached.";
        return false;
    }
    if (!m1600->ce1600fHasDisk()) {
        *error = "There's no disk in the drive.";
        return false;
    }
    if (!fromPreset && !m_isTemplate) {
        *error = inQuotes(m_diskName) + " is already saved; changes are saved automatically.";
        return false;
    }
    std::string newPath;
    const MediaSaveRefusal refusal = m_paths.planNamedSave(MediaKind::Floppy, name, filePath, asTemplate, &newPath);
    if (refusal != MediaSaveRefusal::None) {
        *error = mediaSaveRefusalMessage(refusal, name, newPath);
        return false;
    }
    if (!fromPreset && nameCollides(name)) {
        *error = "A disk named " + inQuotes(name) + " already exists. Choose a different name.";
        return false;
    }
    if (!atomicWriteFile(newPath, formatFloppyFile(name, m1600->ce1600fDiskImage(), asTemplate))) {
        *error = "Couldn't write " + inQuotes(newPath) + ".";
        return false;
    }

    // Retarget the drive at the saved copy: it now shows under its new name
    // and -- unless it was saved as a template -- autosaves there.
    m_diskName = name;
    classifySource(newPath, asTemplate);
    m_persistPending = false;
    m_lastSeenRevision = m1600->ce1600fRevision();
    return true;
}

void FloppyBay::writeInstance() {
    if (m_instanceFilePath.empty()) return;
    PC1600Machine* m1600 = drive();
    if (!m1600) return;
    atomicWriteFile(m_instanceFilePath, formatFloppyFile(m_diskName, m1600->ce1600fDiskImage()));
}

void FloppyBay::markDirty() {
    PC1600Machine* m1600 = drive();
    if (!m1600) return;
    const std::uint64_t rev = m1600->ce1600fRevision();
    if (rev == m_lastSeenRevision) return;
    m_lastSeenRevision = rev;
    if (m_instanceFilePath.empty()) return;  // dirty, but nothing to autosave to yet
    m_persistPending = true;
    m_debounceArmed = true;
}

void FloppyBay::tick(double nowSeconds) {
    if (!m_debounceArmed) return;
    if (!m_debounceDue) {
        // Not restarted by further writes, so a long disk operation still
        // autosaves every kPersistDebounceSeconds.
        m_debounceDue = nowSeconds + kPersistDebounceSeconds;
        return;
    }
    if (nowSeconds >= *m_debounceDue) flushPendingPersist();
}

bool FloppyBay::canNameAndSave() const {
    PC1600Machine* m1600 = m_session.pc1600();
    return m1600 && m1600->ce1600fHasDisk() && m_isTemplate;
}

int FloppyBay::side() const {
    PC1600Machine* m1600 = m_session.pc1600();
    return m1600 ? m1600->ce1600fSide() : 0;
}

void FloppyBay::toggleSide() {
    PC1600Machine* m1600 = drive();
    if (!m1600) return;
    m1600->ce1600fSetSide(m1600->ce1600fSide() == 0 ? 1 : 0);
    m_lastSeenRevision = m1600->ce1600fRevision();
}

bool FloppyBay::motorOn() const {
    PC1600Machine* m1600 = m_session.pc1600();
    return m1600 && m1600->ce1600fMotorOn();
}

void FloppyBay::flushPendingPersist() {
    m_debounceArmed = false;
    m_debounceDue.reset();
    if (!m_persistPending) return;
    writeInstance();
    m_persistPending = false;
}

} // namespace shared
