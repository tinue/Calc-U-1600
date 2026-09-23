#pragma once
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>
#include <cstdint>
#include <vector>

#include "MachineController.hpp"
#include "Connector/MemoryCardDefinition.hpp"  // CardHost

// Owns all memory-module selection/attach/battery-save state for the
// control bar's per-slot pickers -- kept separate from MachineController
// so that facade stays a thin Core wrapper. Slot 1 is always meaningful
// (PC-1500/1500A's single expansion connector, or PC-1600 Slot 1); slot 2
// is only meaningful on PC-1600.
//
// Module (re)selection is a rebuild-time operation, not a live-swap --
// MachineController::switchModel() already reconstructs the whole
// PC1500Machine/PC1600Machine from scratch on every model switch, and
// calls attachAllToFreshMachine() right after loading ROMs, before its
// own reset()/allReset(). Callers that want to change a slot's module
// call selectModule() (updates state only) then re-trigger
// MachineController::switchModel(currentModel()) to force that rebuild.
class MemoryModuleManager : public QObject {
    Q_OBJECT
public:
    explicit MemoryModuleManager(MachineController* controller, QObject* parent = nullptr);

    struct ModuleEntry {
        QString moduleName;
        bool battery = false;
        bool rom = false;  // a ROM module -- listed in its own picker section
    };

    // Catalogue for ControlBar's combo-box population, filtered by
    // compatible-hosts for the given CardHost. `templates` are the files
    // declaring `template: true` -- every bundled card plus any the user
    // dropped into the storage folder; `instances` are the storage
    // folder's other cards (saved instances). A storage-folder card
    // sharing a bundled card's name is left out of both: lookup is
    // bundled-first, so it could never be loaded.
    struct ModuleLists {
        QVector<ModuleEntry> templates;
        QVector<ModuleEntry> instances;
    };
    ModuleLists moduleLists(CardHost host) const;

    // slot is 1 or 2 (PC1500/1500A use only slot 1). Empty moduleName =>
    // detach. Flushes any pending autosave for the slot being replaced
    // before switching.
    void selectModule(int slot, const QString& moduleNameOrEmpty);
    QString selectedModuleName(int slot) const;
    bool slotHasInstanceFile(int slot) const;  // whether autosave-eligible now
    // Name & Save is offered only for a battery-backed template; an
    // instance is kept up to date by autosave instead.
    bool canNameAndSave(int slot) const {
        const SlotState& st = m_slots[slot - 1];
        return !st.moduleName.isEmpty() && st.isTemplate && st.battery;
    }

    // Called by MachineController::switchModel() right after the new
    // machine + ROMs are constructed, BEFORE its reset()/allReset().
    void attachAllToFreshMachine();

    // Called by PresetController after a preset finishes attaching its own
    // module(s) directly (PC1500PresetLoader.cpp / PC1600PresetLoader.cpp
    // already did the live attach -- this only updates this manager's
    // display/bookkeeping state to match, exactly as attachOneSlot() would
    // have. The module's name is read from the machine's slot itself
    // (ExpansionCard::moduleName()) -- an empty slot clears the selection.
    // `resolvedPathOrEmpty` is
    // the on-disk file a `modulespec:`/`modulespecfile:` reference resolved
    // to (PresetLoadResult::expansionModuleResolvedPath /
    // PC1600PresetLoadResult::slot1ResolvedPath/slot2ResolvedPath), empty
    // for an empty slot. The file itself says what it is (classifySlot()):
    // an instance becomes autosave-eligible exactly as if it had been
    // picked from the GUI dropdown; a template never does.
    void syncFromPresetLoad(int slot, const QString& resolvedPathOrEmpty = QString());

    // "Name & Save" flow. A template's name is always refused, whatever
    // host it targets (a bundled card would shadow the saved one on lookup,
    // and a user template must never be overwritten).
    bool nameAndSave(int slot, const QString& instanceName, QString* error);

    // Preset `saveas:s1:<name>` / `saveas:s2:<name>` (PresetController's
    // PresetSaveAsFn callback). Like nameAndSave(),
    // the slot is then retargeted at the new instance (shown under that
    // name, autosaving there), but unlike it: works even when the slot is
    // already saved (a "save as" -- the previous instance file just stops
    // being autosaved), and silently overwrites an existing instance file
    // of the same name instead of refusing. A template's name is still
    // refused, and a template file is never overwritten.
    bool saveAsFromPreset(int slot, const QString& instanceName, QString* error);

    void markDirtyAndSchedulePersist();  // called once per frame tick
    void flushPendingPersist();          // called before select/model-switch/quit

    // Reacts to a model switch: nothing carries over from the previous
    // model, so both slots are cleared (after saving any pending battery-
    // card write) -- the new model starts empty, or with whatever its
    // default preset (AppSettings::defaultPresetPath()) attaches. Called
    // BEFORE MachineController::switchModel().
    void onModelChanged();

    // Which CardHost a slot resolves to under `model` -- shared with
    // MainWindow::refreshModuleCombos() so the slot/model -> host mapping
    // has one owner.
    static CardHost hostForModel(int slot, Model model);

signals:
    void moduleChanged(int slot);
    void errorMessage(const QString& text);

private:
    // Shared body of nameAndSave()/saveAsFromPreset(); `fromPreset` skips
    // the already-saved and name-collision checks.
    bool saveSlotAs(int slot, const QString& instanceName, bool fromPreset, QString* error);
    // Every template's module-name (bundled or in the storage folder, all
    // hosts), and every bundled card's module-name.
    QSet<QString> templateNames() const;
    QSet<QString> bundledNames() const;
    bool nameCollides(const QString& instanceName) const;

    MachineController* m_controller;  // not owned

    struct SlotState {
        QString moduleName;        // empty = no module selected
        QString sourcePath;        // the file the card was loaded from (template or instance)
        bool isTemplate = false;   // that file declares `template: true`
        bool battery = false;      // that file declares `battery: true`
        QString instanceFilePath;  // == sourcePath for an instance (autosaved there), else empty
        bool persistPending = false;
    };
    // Records `resolvedPath` as the slot's source and classifies it from the
    // file's own `template:` key. Empty path = no file (clears the source).
    static void classifySlot(SlotState& st, const QString& resolvedPath);
    SlotState m_slots[2];  // index 0 = slot 1, index 1 = slot 2

    QTimer* m_debounceTimer = nullptr;  // single-shot, 500ms, restarted while dirty

    CardHost hostFor(int slot) const;
    bool currentSlotImage(int slot, int* bankCount, std::vector<uint8_t>* image) const;
    void writeInstance(int slot);  // re-splice + rewrite slot's instance file

    // Reads `sourcePath`, splices slot `slot`'s live image into it as
    // `targetName` (attributed to `sourceModuleName` for the template
    // lookup), and returns the spliced text via `*spliced`. Shared by
    // nameAndSave() (surfaces failures via `error`) and writeInstance()
    // (best-effort re-splice; pass `error` as nullptr to fail silently).
    bool spliceCardImageInto(int slot, const QString& sourcePath, const QString& sourceModuleName,
                              const QString& targetName, std::string* spliced, QString* error);

    // The name of the module in `slot` of the live machine, "" when empty.

    QString attachedModuleName(int slot) const;

    template <typename AttachFn>
    void attachOneSlot(int slotIndex, CardHost host, AttachFn attach);
};
