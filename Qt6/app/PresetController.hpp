#pragma once
#include <QObject>
#include <QString>
#include <cstdint>
#include <functional>
#include <vector>

#include "MachineController.hpp"  // Model

class MachineController;
class MemoryModuleManager;
class FloppyDiskManager;
struct PresetFile;
struct PresetLoadResult;

// Orchestrates opening a preset file end to end (Core/PC1500/
// PresetFile.hpp, PC1500PresetLoader.cpp / PC1600PresetLoader.cpp): parses
// the preset, constructs+ROM-loads whichever machine model it targets via
// MachineController's preset-construction hooks (resetBareForPreset*/
// finishPresetLoad), applies it, then syncs MemoryModuleManager's slot
// bookkeeping to match what the preset itself attached.
//
// Runs synchronously on the calling (UI) thread. MainWindow stops the frame
// timer around the call so it can't reenter the machine mid-load, and
// installs a yield hook (setYieldHook()) that the machine calls
// periodically from inside its run loop, so the window keeps repainting
// and can show a "Loading..." popup instead of freezing.
//
// Only meaningful when CALCU1600_PRESET_LOADER_AVAILABLE is defined
// (macOS for now -- see Qt6/CMakeLists.txt's CORE_SOURCES if(APPLE) block:
// BASIC program loading needs the vendored Rust libsharpdx, currently only
// built for macOS). MainWindow checks that macro itself and disables the
// "Load Preset…" action instead of constructing this class when it's
// undefined, so this header is safe to include unconditionally.
class PresetController : public QObject {
    Q_OBJECT
public:
    PresetController(MachineController* controller, MemoryModuleManager* moduleManager,
                     FloppyDiskManager* floppyManager, QObject* parent = nullptr);

    // Loads the preset at `path`. On success (or a failure partway through
    // the preset's own keys:/program: steps -- see PresetLoadResult's
    // doc), MachineController's live model may have changed to whatever
    // the preset targeted; the caller should refresh any model/module UI
    // regardless of the return value. Returns false with *error set to a
    // user-facing message on failure (parse error, unsupported ROM
    // revision, a missing/rejected sibling file, etc).
    bool loadPreset(const QString& path, QString* error);

    // A model's default preset (AppSettings::defaultPresetPath()): same as
    // loadPreset(), but refuses -- touching nothing -- a preset that targets
    // a different model than `model`, so a misconfigured default can't
    // silently switch the user to another machine.
    bool loadDefaultPreset(const QString& path, Model model, QString* error);

    // Loads a plain `.bas` listing directly into the *currently running*
    // machine -- no preset wrapper, no model/ROM/module rebuild (only a
    // reset). Runs the same "reset, reach PRO mode, NEW0, poke the
    // tokenized payload in" choreography a preset's `format: basic-binary`
    // program section relies on its own `keys:` block for (see
    // PC1500BasicLoader.hpp/PC1600BasicLoader.hpp's own doc comments), just
    // driven here instead of by preset steps. Synchronous on the calling
    // thread, same caller contract as loadPreset() (stop the frame timer
    // first). Resets the machine and destroys the current program (NEW0).
    bool loadBasicProgramLive(const QString& path, QString* error);

    // Writes a machine-code block (File > Load Machine Code…) into the
    // *currently running* machine -- no reset, no BASIC involvement, just
    // the bytes. `slot` is PC-1600 only: 0 = S0, 1 / 2 = memory slots. Same
    // caller contract as the loaders above (stop the frame timer first).
    // Doesn't need libsharpdx, so it works in every build.
    struct MachineCodeLoadRequest {
        std::vector<uint8_t> payload;
        uint32_t addr = 0;
        int slot = 0;
    };
    bool loadMachineCodeLive(const MachineCodeLoadRequest& request, QString* error);

    // The GUI's Reset / Reset All: MachineController::resetToPrompt() with
    // the yield hook installed, so the flat-out boot keeps the window
    // painting. Same caller contract as the loaders above. Works in every
    // build.
    bool resetLive(bool allReset, QString* error);
    // MachineController::powerCycleAround() (plotter attach/detach) with the
    // yield hook installed, like resetLive().
    bool powerCycleLive(const std::function<void()>& change, QString* error);

    // Callback installed on the target machine (PC1500Machine/
    // PC1600Machine::setYieldHook()) for the duration of each load above,
    // then removed again. Called on the calling thread roughly every few
    // ms of emulated time; it must not drive the machine. Empty = none.
    void setYieldHook(std::function<void()> hook) { m_yieldHook = std::move(hook); }

signals:
    // Fired exactly once per loadPreset() call that gets far enough to
    // attach the preset's model/cards/plotter, right before the machine
    // boots -- i.e. while it's fully armed but still powered off.
    // MemoryModuleManager's slot bookkeeping is already synced by the time
    // this fires (see loadPreset()'s onArmed lambda), so a slot connected
    // here can safely refresh module combos and plotter-paper visibility
    // and repaint before the (possibly many-seconds-long) boot and preset
    // script run. Also fired when arming fails part way (a bad modulespec,
    // a missing plotter ROM, ...), after the slot/floppy pickers were
    // synced to what did attach; never for a preset that fails to parse.
    void armed();

private:
    // Shared tail of loadPreset()/loadDefaultPreset() once the file parsed:
    // picks the model's run method, then reports its outcome.
    bool runPreset(const PresetFile& preset, QString* error);
    // Per-model: build the bare machine, announce the model, apply the
    // preset. `env` is the model-independent part (search dirs, log, saveas:).
    struct PresetEnv;
    PresetLoadResult runPC1500Preset(const PresetFile& preset, const PresetEnv& env);
    PresetLoadResult runPC1600Preset(const PresetFile& preset, const PresetEnv& env);

    // Runs `body` with the yield hook installed on whichever machine is
    // active (the ScopedYieldHook template argument is the only thing the
    // two branches differ in); false + `error` with no machine.
    bool withYieldHook(const std::function<void()>& body, QString* error);

    MachineController* m_controller;       // not owned
    MemoryModuleManager* m_moduleManager;  // not owned
    FloppyDiskManager* m_floppyManager;    // not owned
    std::function<void()> m_yieldHook;
};
