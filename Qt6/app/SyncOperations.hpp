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

    /// Runs `op`. A failure goes to `error`, or to a warning box titled
    /// `title` when `error` is null. Nested calls run straight through.
    bool run(const QString& title, job::Task<shared::OpResult> op, QString* error = nullptr);
    /// The same for an instant operation (no emulated time).
    bool run(const QString& title, const std::function<shared::OpResult()>& op, QString* error = nullptr);

    // The common operations.
    /// `armOnly`: the machine set up but not booted (PresetFile::armOnly).
    bool loadPreset(const QString& path, QString* error = nullptr, bool armOnly = false);
    /// Refuses a preset for another model (AppSession::loadDefaultPresetJob()).
    bool loadDefaultPreset(const QString& path, Model model, QString* error = nullptr);
    bool resetToPrompt(bool allReset, QString* error = nullptr);

    /// An operation is running (AppSession::busy()); the window resyncs
    /// when it ends (MachineController::appBusyChanged).
    bool busy() const;

private:
    QWidget* m_window;
    MachineController* m_machines;
    EmulationPacer* m_pacer;
    std::function<void()> m_refreshLcd;
};
