#pragma once

#include <QObject>
#include <QString>

#include <functional>

#include "MachineController.hpp"

class EmulationPacer;
class QWidget;

// ── Machine operations, run to completion ────────────────────────────────
//
// A preset, a reset, a model switch, a plotter power cycle, a load: every
// one -- from a menu, the control bar or the debugger -- runs through here.
// The operation itself is a shared::AppSession job; this runs it to
// completion on the GUI thread (AppSession::runNow()), with the frame timer
// stopped (nothing else may drive the machine meanwhile), the window kept
// painting between job slices (user input excluded), and a "Loading…"
// popup when it takes long. Afterwards the frame timer restarts. Pending
// card/disk writes go first, and the stale audio and the clock are dealt
// with, inside AppSession. busyChanged() tells listeners (the debugger's
// request queue) when the machine is being driven this way.
class SyncOperations : public QObject {
    Q_OBJECT
public:
    SyncOperations(QWidget* window, MachineController* machines, EmulationPacer* pacer,
                   std::function<void()> refreshLcd, QObject* parent = nullptr);

    /// Runs `op`, then `afterLoad`. A failure goes to `error`, or to a
    /// warning box titled `title` when `error` is null. Nested calls run
    /// straight through.
    bool run(const QString& title, job::Task<shared::OpResult> op, const std::function<void()>& afterLoad = {},
             QString* error = nullptr);
    /// The same for an instant operation (no emulated time).
    bool run(const QString& title, const std::function<bool(QString*)>& op,
             const std::function<void()>& afterLoad = {}, QString* error = nullptr);

    /// Resyncs the UI after a preset (it may have attached peripherals and
    /// rebuilt the machine); also runs when a preset failed before arming.
    void setPresetResync(std::function<void()> resync) { m_presetResync = std::move(resync); }

    // The common operations.
    /// `armOnly`: the machine set up but not booted (PresetFile::armOnly).
    bool loadPreset(const QString& path, QString* error = nullptr, bool armOnly = false);
    /// Refuses a preset for another model (AppSession::loadDefaultPresetJob()).
    bool loadDefaultPreset(const QString& path, Model model, QString* error = nullptr);
    bool resetToPrompt(bool allReset, QString* error = nullptr);

    bool busy() const { return m_running; }

signals:
    void busyChanged(bool busy);

private:
    QWidget* m_window;
    MachineController* m_machines;
    EmulationPacer* m_pacer;
    std::function<void()> m_refreshLcd;
    std::function<void()> m_presetResync;
    bool m_running = false; // run() doesn't nest: an inner call runs straight through
};
