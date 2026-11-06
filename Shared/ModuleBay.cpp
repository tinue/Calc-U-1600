#include "Shared/ModuleBay.hpp"

#include <filesystem>
#include <set>
#include <system_error>

#include "Connector/MediaSave.hpp"
#include "Connector/MemoryCardCatalog.hpp"
#include "Connector/SoftwareDefinedCard.hpp"
#include "Preset/PresetRunner.hpp"
#include "Shared/AppPaths.hpp"
#include "Shared/MachineSession.hpp"
#include "Shared/Text.hpp"
#include "Shared/Utf8Path.hpp"

namespace shared {

namespace {

ModuleBay::ModuleEntry entryFrom(const MemoryCardCatalogEntry& e) { return {e.moduleName, e.battery, e.rom}; }

} // namespace

ModuleBay::ModuleBay(MachineSession& session, const AppPaths& paths, Events events)
    : m_session(session), m_paths(paths), m_events(std::move(events)) {}

CardHost ModuleBay::hostFor(int slot) const { return hostForModel(slot, m_session.currentModel()); }

CardHost ModuleBay::hostForModel(int slot, Model model) {
    if (model == Model::PC1600) return slot == 2 ? CardHost::PC1600Slot2 : CardHost::PC1600Slot1;
    return model == Model::PC1500A ? CardHost::PC1500A : CardHost::PC1500;
}

ModuleBay::ModuleLists ModuleBay::moduleLists(CardHost host) const {
    ModuleLists lists;
    std::set<std::string> bundled;
    for (const auto& e : scanMemoryCardDirectory(m_paths.bundledResourcesDir(), nullptr)) {
        bundled.insert(e.moduleName);
        if (e.compatibleWith(host)) lists.templates.push_back(entryFrom(e));
    }
    for (const auto& e : scanMemoryCardDirectory(m_paths.instanceDir(), nullptr)) {
        if (!e.compatibleWith(host) || bundled.count(e.moduleName)) continue;
        (e.isTemplate ? lists.templates : lists.instances).push_back(entryFrom(e));
    }
    return lists;
}

void ModuleBay::selectModule(int slot, const std::string& moduleNameOrEmpty) {
    const int idx = slot - 1;
    flushPendingPersist();
    m_slots[idx] = SlotState{};
    m_slots[idx].moduleName = moduleNameOrEmpty;
}

void ModuleBay::attachOneSlot(int slot) {
    SlotState& st = m_slots[slot - 1];
    const CardHost host = hostFor(slot);
    const auto attach = [&](std::unique_ptr<ExpansionCard> card) { m_session.attachSlotCard(slot, std::move(card)); };
    if (!st.romFile.empty()) {
        // A preset's ROM module under development: the same file and ROM
        // again (read anew), never autosaved.
        std::string resolved, err;
        std::unique_ptr<ExpansionCard> card = makePresetModuleCard(st.sourcePath, {}, st.romFile, {}, host, &resolved, &err);
        if (card) {
            attach(std::move(card));
            return;
        }
        reportError("Couldn't attach " + inQuotes(st.moduleName) + ": " + err);
        st = SlotState{};
        return;
    }
    std::string path, err;
    if (resolveModuleSpecByName({m_paths.bundledResourcesDir(), m_paths.instanceDir()}, st.moduleName, &path, &err)) {
        auto card = makeSoftwareDefinedCard(path, host, &err);
        if (card) {
            classifySlot(st, path);
            attach(std::move(card));
            // Freshly loaded from the file -- identical to it until the
            // machine writes to the card.
            if (!st.instanceFilePath.empty()) st.persistedRevision = currentSlotRevision(slot);
            return;
        }
    }
    reportError("Couldn't attach " + inQuotes(st.moduleName) + ": " + err);
    st = SlotState{};
}

void ModuleBay::classifySlot(SlotState& st, const std::string& resolvedPath) const {
    st.sourcePath = resolvedPath;
    st.isTemplate = false;
    st.battery = false;
    st.instanceFilePath.clear();
    if (resolvedPath.empty()) return;
    MemoryCardCatalogEntry entry;
    if (readMemoryCardCatalogEntry(resolvedPath, &entry, nullptr)) {
        st.isTemplate = entry.isTemplate;
        st.battery = entry.battery;
    }
    // An instance autosaves in place, wherever it was loaded from -- but
    // never into the bundle, even if a bundled file lacked `template: true`.
    if (!st.isTemplate && !isUnderDir(resolvedPath, m_paths.bundledResourcesDir())) st.instanceFilePath = resolvedPath;
}

void ModuleBay::attachAllToFreshMachine() {
    m_session.detachSlots();  // no-op on a fresh machine; defensive/self-documenting
    for (int slot = 1; slot <= m_session.slotCount(); ++slot)
        if (!m_slots[slot - 1].moduleName.empty()) attachOneSlot(slot);
}

std::string ModuleBay::attachedModuleName(int slot) const {
    const ExpansionCard* card = m_session.slotCard(slot);
    return card ? card->moduleName() : std::string();
}

void ModuleBay::syncFromPresetLoad(int slot, const std::string& resolvedPathOrEmpty, const std::string& romFile) {
    const int idx = slot - 1;
    m_slots[idx] = SlotState{};
    m_slots[idx].moduleName = attachedModuleName(slot);
    if (!m_slots[idx].moduleName.empty()) {
        classifySlot(m_slots[idx], resolvedPathOrEmpty);
        m_slots[idx].romFile = romFile;
        if (!romFile.empty()) m_slots[idx].instanceFilePath.clear();
    }
    if (m_events.moduleChanged) m_events.moduleChanged(slot);
}

bool ModuleBay::nameCollides(const std::string& instanceName) const {
    std::error_code ec;
    if (std::filesystem::exists(toPath(m_paths.instancePathFor(instanceName)), ec)) return true;
    std::string err;
    for (const auto& e : scanMemoryCardDirectory(m_paths.instanceDir(), &err))
        if (e.moduleName == instanceName) return true;
    return false;
}

bool ModuleBay::nameAndSave(int slot, const std::string& instanceName, std::string* error) {
    return saveSlotAs(slot, instanceName, /*fromPreset=*/false, error);
}

bool ModuleBay::saveAsFromPreset(int slot, const std::string& instanceName, const std::string& filePath,
                                 bool asTemplate, std::string* error) {
    return saveSlotAs(slot, instanceName, /*fromPreset=*/true, error, filePath, asTemplate);
}

bool ModuleBay::saveSlotAs(int slot, const std::string& instanceName, bool fromPreset, std::string* error,
                           const std::string& filePath, bool asTemplate) {
    const std::string name = trimmed(instanceName);
    if (name.empty()) {
        *error = "Name cannot be empty.";
        return false;
    }
    SlotState& st = m_slots[slot - 1];
    if (st.moduleName.empty()) {
        *error = "No module attached.";
        return false;
    }
    if (!st.romFile.empty()) {
        *error = "The ROM of " + inQuotes(st.moduleName) + " comes from " + st.romFile +
                 ", not from its module file, so it can't be saved.";
        return false;
    }
    if (!fromPreset && !st.isTemplate) {
        *error = inQuotes(st.moduleName) + " is already saved; changes are saved automatically.";
        return false;
    }
    if (st.sourcePath.empty()) {
        *error = inQuotes(st.moduleName) + " has no source file to save from.";
        return false;
    }
    std::string newPath;
    const MediaSaveRefusal refusal = m_paths.planNamedSave(MediaKind::Card, name, filePath, asTemplate, &newPath);
    if (refusal != MediaSaveRefusal::None) {
        *error = mediaSaveRefusalMessage(refusal, name, newPath);
        return false;
    }
    if (!fromPreset && nameCollides(name)) {
        *error = "A card named " + inQuotes(name) + " already exists. Choose a different name.";
        return false;
    }

    // Splice from the file the card was loaded from -- a template, or (a
    // preset save-as) an instance; either way its layout is the card's.
    const ExpansionCard* card = m_session.slotCard(slot);
    if (!card) {
        *error = "Couldn't read the live card contents.";
        return false;
    }
    const std::uint64_t revision = card->contentRevision();
    std::string spliced, splErr;
    if (!formatCardSave(*card, st.sourcePath, name, asTemplate, &spliced, &splErr)) {
        *error = "Couldn't generate the instance file: " + splErr;
        return false;
    }
    if (!atomicWriteFile(newPath, spliced)) {
        *error = "Couldn't write " + inQuotes(newPath) + ".";
        return false;
    }

    // Retarget the slot at the saved copy: it now shows under its new name
    // and -- unless it was saved as a template -- autosaves there.
    st.moduleName = name;
    st.sourcePath = newPath;
    st.isTemplate = asTemplate;  // `battery` carries over
    st.instanceFilePath = asTemplate ? std::string() : newPath;
    st.persistedRevision = revision;
    if (m_events.moduleChanged) m_events.moduleChanged(slot);
    return true;
}

void ModuleBay::writeInstance(int slot) {
    SlotState& st = m_slots[slot - 1];
    if (st.instanceFilePath.empty()) return;

    // Runs on every debounce: skip the image copy, file read and rewrite
    // unless the card's content changed since it was last attached from or
    // written to the file.
    const ExpansionCard* card = m_session.slotCard(slot);
    if (!card) return;
    const std::uint64_t revision = card->contentRevision();
    if (st.persistedRevision == revision) return;

    std::string spliced, err;
    if (!formatCardSave(*card, st.instanceFilePath, st.moduleName, false, &spliced, &err)) return;
    if (atomicWriteFile(st.instanceFilePath, spliced)) st.persistedRevision = revision;
}

std::uint64_t ModuleBay::currentSlotRevision(int slot) const {
    const ExpansionCard* card = m_session.slotCard(slot);
    return card ? card->contentRevision() : 0;
}

void ModuleBay::tick(double nowSeconds) {
    if (!m_persistDue) {
        // The first change starts the debounce; later ones coalesce.
        for (int slot = 1; slot <= 2; ++slot) {
            const SlotState& st = m_slots[slot - 1];
            if (!st.instanceFilePath.empty() && st.persistedRevision != currentSlotRevision(slot)) {
                m_persistDue = nowSeconds + kPersistDebounceSeconds;
                break;
            }
        }
        return;
    }
    if (nowSeconds >= *m_persistDue) flushPendingPersist();
}

void ModuleBay::flushPendingPersist() {
    m_persistDue.reset();
    writeInstance(1);
    writeInstance(2);
}

void ModuleBay::onModelChanged() {
    flushPendingPersist();
    for (auto& st : m_slots) st = SlotState{};
}

} // namespace shared
