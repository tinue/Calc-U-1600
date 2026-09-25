#pragma once
#include <QElapsedTimer>
#include <QObject>
#include <functional>

class AudioOutput;
class MachineController;
class QTimer;

// Drives the emulation from the GUI thread: a ~60 Hz frame timer that runs
// exactly the emulated time that passed on the wall clock (or, while the
// LCD is held for turbo, as much as the host can produce), then pumps the
// buzzer audio and has the window refresh its views. The --shots runner
// freezes it instead and advances emulated time explicitly (runFor(),
// runUntilPasteDone()), with the same view refresh.
class EmulationPacer : public QObject {
    Q_OBJECT
public:
    EmulationPacer(MachineController* controller, AudioOutput* audio, std::function<void()> refreshViews,
                   QObject* parent = nullptr);

    /// Stop the frame timer for a synchronous load (nothing else may drive
    /// the machine meanwhile) / restart it afterwards, unless frozen.
    void suspend();
    void resume();
    /// Rebase real-time pacing on "now" -- after anything that blocked the
    /// frame timer (a rebuild's flat-out boot), so the next tick doesn't
    /// try to catch up.
    void restartPacing();

    void setTurbo(bool active) { m_turbo = active; }

    /// Frozen: no frame timer at all; emulated time only moves via runFor()
    /// / runUntilPasteDone(), so a capture is reproducible run to run.
    void setFrozen(bool frozen);
    /// Runs `seconds` of emulated time in frame-sized slices, then refreshes
    /// the views.
    void runFor(double seconds);
    /// Runs until a MachineController::pasteText() has been fully typed;
    /// false if it is still typing after `capSeconds` of emulated time.
    bool runUntilPasteDone(double capSeconds);

private:
    void onTick();

    MachineController* m_controller; // not owned
    AudioOutput* m_audio;            // not owned
    std::function<void()> m_refreshViews;
    QTimer* m_timer = nullptr;
    bool m_turbo = false;
    bool m_frozen = false;
    QElapsedTimer m_paceClock;
    double m_cycleCarry = 0.0;
};
