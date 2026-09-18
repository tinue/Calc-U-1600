#pragma once
#include <QObject>
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
    };

    // Catalogue queries for ControlBar's combo-box population, filtered
    // by compatible-hosts for the given CardHost.
    QVector<ModuleEntry> bundledEntries(CardHost host) const;
    QVector<ModuleEntry> instanceEntries(CardHost host) const;

    // slot is 1 or 2 (PC1500/1500A use only slot 1). Empty moduleName =>
    // detach. Flushes any pending autosave for the slot being replaced
    // before switching.
    void selectModule(int slot, const QString& moduleNameOrEmpty);
    QString selectedModuleName(int slot) const;
    bool isSlotBatteryBacked(int slot) const;
    // Same lookup, but against already-fetched bundled/instance entries --
    // for callers (e.g. combo-box refresh) that fetched them anyway and
    // would otherwise trigger a second pair of directory scans.
    bool isSlotBatteryBacked(int slot, const QVector<ModuleEntry>& bundled,
                              const QVector<ModuleEntry>& instance) const;
    bool slotHasInstanceFile(int slot) const;  // whether autosave-eligible now

    // Called by MachineController::switchModel() right after the new
    // machine + ROMs are constructed, BEFORE its reset()/allReset().
    void attachAllToFreshMachine();

    // Called by PresetController after a preset finishes attaching its own
    // module(s) directly (PC1500PresetLoader.cpp / PC1600PresetLoader.cpp
    // already did the live attach -- this only updates this manager's
    // display/bookkeeping state to match, exactly as attachOneSlot() would
    // have. `labelOrEmpty` is PresetLoadResult::expansionModuleLabel
    // (PC-1500) or slot1ModuleLabel/slot2ModuleLabel (PC-1600) -- empty
    // means the preset left the slot unpopulated. `resolvedPathOrEmpty` is
    // the on-disk file a `modulespec:`/`modulespecfile:` reference resolved
    // to (PresetLoadResult::expansionModuleResolvedPath /
    // PC1600PresetLoadResult::slot1ResolvedPath/slot2ResolvedPath), empty
    // for a built-in `- module: <name>` or an empty slot: when it names a
    // file actually under AppPaths::instanceDir() (a real saved battery
    // instance, not a bundled read-only template), the slot becomes
    // autosave-eligible exactly as if the same instance had been picked
    // from the GUI dropdown (attachOneSlot()'s own instanceFilePath rule).
    void syncFromPresetLoad(int slot, const QString& labelOrEmpty,
                            const QString& resolvedPathOrEmpty = QString());

    // "Name & Save" flow.
    bool nameCollides(const QString& instanceName) const;
    bool nameAndSave(int slot, const QString& instanceName, QString* error);

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
    MachineController* m_controller;  // not owned

    struct SlotState {
        QString moduleName;        // empty = no module selected
        QString instanceFilePath;  // empty unless resolved from the instance dir
        bool persistPending = false;
    };
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

    template <typename AttachFn>
    void attachOneSlot(int slotIndex, CardHost host, AttachFn attach);
};
