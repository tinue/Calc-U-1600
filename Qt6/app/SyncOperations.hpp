#pragma once

#include <QObject>
#include <QString>

#include <functional>

#include "MachineController.hpp"

class EmulationPacer;
class FloppyDiskManager;
class MemoryModuleManager;
class PresetController;
class QWidget;

// ── Synchronous machine operations ───────────────────────────────────────
//
// A preset, a BASIC or machine-code load, a reset: operations that drive
// the machine flat out on the GUI thread until they're done. Every one --
// from a menu, the control bar or the debugger -- runs through run(), which
// stops the frame timer (nothing else may drive the machine meanwhile),
// saves pending card/disk writes first, keeps the window painting from the
// machine's yield hook (user input excluded), shows a "Loading…" popup when
// it takes long, and afterwards drops the stale audio and restarts pacing
// (which seeds the clock from the host). busyChanged() tells listeners (the
// debugger's request queue) when the machine is being driven this way.
class SyncOperations : public QObject {
    Q_OBJECT
public:
    SyncOperations(QWidget* window, MachineController* machines, PresetController* presets, EmulationPacer* pacer,
                   MemoryModuleManager* modules, FloppyDiskManager* floppies, std::function<void()> refreshLcd,
                   QObject* parent = nullptr);

    /// Runs `op`, then `afterLoad`. A failure goes to `error`, or to a
    /// warning box titled `title` when `error` is null. Nested calls only
    /// run their op.
    bool run(const QString& title, const std::function<bool(QString*)>& op, const std::function<void()>& afterLoad = {},
             QString* error = nullptr);

    /// Resyncs the UI after a preset (it may have attached peripherals and
    /// rebuilt the machine); also runs when a preset failed before arming.
    void setPresetResync(std::function<void()> resync) { m_presetResync = std::move(resync); }

    // The common operations.
    bool loadPreset(const QString& path, QString* error = nullptr);
    /// Refuses a preset for another model (see PresetController::loadDefaultPreset()).
    bool loadDefaultPreset(const QString& path, Model model, QString* error = nullptr);
    bool resetToPrompt(bool allReset, QString* error = nullptr);

    bool busy() const { return m_depth > 0; }

signals:
    void busyChanged(bool busy);

private:
    QWidget* m_window;
    MachineController* m_machines;
    PresetController* m_presets;
    EmulationPacer* m_pacer;
    MemoryModuleManager* m_modules;
    FloppyDiskManager* m_floppies;
    std::function<void()> m_refreshLcd;
    std::function<void()> m_presetResync;
    int m_depth = 0;
};
