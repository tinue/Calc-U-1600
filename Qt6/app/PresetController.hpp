#pragma once
#include <QObject>
#include <QString>

class MachineController;
class MemoryModuleManager;

// Orchestrates opening a preset file end to end (Core/PC1500/
// PresetFile.hpp, PC1500PresetLoader.cpp / PC1600PresetLoader.cpp): parses
// the preset, constructs+ROM-loads whichever machine model it targets via
// MachineController's preset-construction hooks (resetBareForPreset*/
// finishPresetLoad), applies it, then syncs MemoryModuleManager's slot
// bookkeeping to match what the preset itself attached.
//
// Runs synchronously on the calling (UI) thread -- there's no threading
// infrastructure here, so a preset with long `wait:` steps will visibly
// freeze the window for its duration. MainWindow stops the frame timer
// around the call so it can't reenter the machine mid-load; that's the
// whole mitigation for now.
//
// Only meaningful when CALCU1600_PRESET_LOADER_AVAILABLE is defined
// (macOS for now -- see Qt6/CMakeLists.txt's CORE_SOURCES if(APPLE) block:
// BASIC program loading needs the vendored Rust libsharpdx, currently only
// built for macOS). MainWindow checks that macro itself and disables the
// "Open Preset…" action instead of constructing this class when it's
// undefined, so this header is safe to include unconditionally.
class PresetController : public QObject {
    Q_OBJECT
public:
    PresetController(MachineController* controller, MemoryModuleManager* moduleManager, QObject* parent = nullptr);

    // Loads the preset at `path`. On success (or a failure partway through
    // the preset's own keys:/program: steps -- see PresetLoadResult's
    // doc), MachineController's live model may have changed to whatever
    // the preset targeted; the caller should refresh any model/module UI
    // regardless of the return value. Returns false with *error set to a
    // user-facing message on failure (parse error, unsupported ROM
    // revision, a missing/rejected sibling file, etc).
    bool loadPreset(const QString& path, QString* error);

signals:
    // Fired exactly once per loadPreset() call that gets far enough to
    // attach the preset's model/cards/plotter, right before the machine
    // boots -- i.e. while it's fully armed but still powered off.
    // MemoryModuleManager's slot bookkeeping is already synced by the time
    // this fires (see loadPreset()'s onArmed lambda), so a slot connected
    // here can safely refresh module combos and plotter-paper visibility
    // and repaint before the (possibly many-seconds-long) boot and preset
    // script run. Never fired for a preset that fails before arming (a
    // parse error, a bad modulespec, a missing plotter ROM, ...).
    void armed();

private:
    MachineController* m_controller;       // not owned
    MemoryModuleManager* m_moduleManager;  // not owned
};
