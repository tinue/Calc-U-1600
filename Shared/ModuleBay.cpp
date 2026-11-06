#include "Shared/ModuleBay.hpp"

#include <filesystem>
#include <set>
#include <system_error>

#include "Connector/MediaSave.hpp"
#include "Connector/MemoryCardCatalog.hpp"
#include "Connector/SoftwareDefinedCard.hpp"
#include "PC1500/PC1500Machine.hpp"
#include "PC1600/PC1600Machine.hpp"
#include "Preset/PresetRunner.hpp"
#include "Shared/AppPaths.hpp"
#include "Shared/MachineSession.hpp"
#include "Shared/Utf8Path.hpp"

namespace shared {

namespace {

ModuleBay::ModuleEntry entryFrom(const MemoryCardCatalogEntry& e) { return {e.moduleName, e.battery, e.rom}; }

std::string trimmed(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string inQuotes(const std::string& s) { return "\"" + s + "\""; }

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

template <typename AttachFn>
void ModuleBay::attachOneSlot(int slotIndex, CardHost host, AttachFn attach) {
    SlotState& st = m_slots[slotIndex];
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
            if (!st.instanceFilePath.empty()) st.persistedRevision = currentSlotRevision(slotIndex + 1);
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
    if (auto* m1500 = m_session.pc1500()) {
        m1500->expansionConnector().detach();  // no-op on a fresh machine; defensive/self-documenting
        if (!m_slots[0].moduleName.empty()) {
            attachOneSlot(0, hostFor(1),
                          [&](std::unique_ptr<ExpansionCard> card) { m1500->attachExpansionCard(std::move(card)); });
        }
        return;
    }
    if (auto* m1600 = m_session.pc1600()) {
        m1600->detachSlot1();
        m1600->detachSlot2();
        if (!m_slots[0].moduleName.empty())
            attachOneSlot(0, CardHost::PC1600Slot1,
                          [&](std::unique_ptr<ExpansionCard> c) { m1600->attachSlot1Card(std::move(c)); });
        if (!m_slots[1].moduleName.empty())
            attachOneSlot(1, CardHost::PC1600Slot2,
                          [&](std::unique_ptr<ExpansionCard> c) { m1600->attachSlot2Card(std::move(c)); });
    }
}

std::string ModuleBay::attachedModuleName(int slot) const {
    if (auto* m1500 = m_session.pc1500()) {
        const ExpansionCard* card = slot == 1 ? m1500->expansionConnector().attachedCard() : nullptr;
        return card ? card->moduleName() : std::string();
    }
    if (auto* m1600 = m_session.pc1600()) return m1600->memory().slotModuleName(slot);
    return {};
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

const ExpansionCard* ModuleBay::slotCard(int slot) const {
    if (auto* m1500 = m_session.pc1500()) return slot == 1 ? m1500->expansionConnector().attachedCard() : nullptr;
    if (auto* m1600 = m_session.pc1600()) return m1600->memory().slotCard(slot);
    return nullptr;
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
    const ExpansionCard* card = slotCard(slot);
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
    st.persistPending = false;
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
    const ExpansionCard* card = slotCard(slot);
    if (!card) return;
    const std::uint64_t revision = card->contentRevision();
    if (st.persistedRevision == revision) return;

    std::string spliced, err;
    if (!formatCardSave(*card, st.instanceFilePath, st.moduleName, false, &spliced, &err)) return;
    if (atomicWriteFile(st.instanceFilePath, spliced)) st.persistedRevision = revision;
}

std::uint64_t ModuleBay::currentSlotRevision(int slot) const {
    const ExpansionCard* card = slotCard(slot);
    return card ? card->contentRevision() : 0;
}

void ModuleBay::markDirty() {
    for (auto& st : m_slots) {
        if (!st.instanceFilePath.empty()) {
            st.persistPending = true;
            m_debounceArmed = true;
        }
    }
}

void ModuleBay::tick(double nowSeconds) {
    if (!m_debounceArmed) return;
    if (!m_debounceDue) {
        // The first dirty frame starts the debounce; later ones coalesce.
        m_debounceDue = nowSeconds + kPersistDebounceSeconds;
        return;
    }
    if (nowSeconds >= *m_debounceDue) flushPendingPersist();
}

void ModuleBay::flushPendingPersist() {
    m_debounceArmed = false;
    m_debounceDue.reset();
    for (int i = 0; i < 2; ++i) {
        if (!m_slots[i].persistPending) continue;
        writeInstance(i + 1);
        m_slots[i].persistPending = false;
    }
}

void ModuleBay::onModelChanged() {
    for (int i = 0; i < 2; ++i) {
        if (m_slots[i].persistPending) writeInstance(i + 1);
        m_slots[i] = SlotState{};
    }
}

} // namespace shared
