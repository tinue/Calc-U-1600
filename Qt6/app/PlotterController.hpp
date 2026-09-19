#pragma once
#include <QObject>

class MachineController;
class FloppyDiskManager;

// Owns the plotter attach/detach power-cycle sequence: real CE-150/CE-1600P
// hardware requires the calculator to be powered off, connected, then
// powered back on -- not a live hot-plug -- so toggling either plotter here
// synthesizes an OFF keypress, waits for the emulated ROM to actually finish
// powering down, performs the (instantaneous) Core attach/detach call, then
// synthesizes an ON keypress. No machine object is rebuilt (unlike
// MachineController::switchModel()) -- this is a live call into the already-
// running PC1500Machine/PC1600Machine.
//
// The app is single-threaded (it already advances the machine from one
// 16ms QTimer via MainWindow::onFrameTick()), so this is a plain frame-
// counted state machine advanced by onFrameTick() -- no async dispatch or
// polling-with-timeout plumbing needed.
class PlotterController : public QObject {
    Q_OBJECT
public:
    // `floppyManager` puts the selected disk into the CE-1600F whenever the
    // CE-1600P (and with it the floppy) attaches.
    PlotterController(MachineController* controller, FloppyDiskManager* floppyManager, QObject* parent = nullptr);

    void requestToggleCE150();
    void requestToggleCE1600P();

    // Advances the power-cycle state machine by one frame (~16ms); no-op
    // when idle. Called from MainWindow::onFrameTick().
    void onFrameTick();

    // Called from MainWindow's modelSelected handler: switchModel() already
    // rebuilt the machine from scratch (no plotter survives that), so this
    // just resyncs our own idle/detached bookkeeping and cancels any
    // in-flight power-cycle rather than letting it act on the new machine.
    void resetOnModelSwitch();

    // Re-emits ce150AttachedChanged/ce1600pAttachedChanged from whatever
    // MachineController currently reports, without touching the power-cycle
    // state machine. Used after a preset attaches a plotter directly on the
    // Core machine (bypassing attachCE150()/attachCE1600P() and therefore
    // this class entirely), so the GUI picks up the real attach state
    // instead of staying wrong or being force-cleared by
    // resetOnModelSwitch().
    void syncFromMachineState();

signals:
    void ce150AttachedChanged(bool attached);
    void ce1600pAttachedChanged(bool attached);
    // UI disables both plotter toggle buttons while a power-cycle is in
    // flight, matching the real hardware's own "wait for it" constraint.
    void busyChanged(bool busy);

private:
    enum class Step { Idle, HoldingOff, WaitingPoweredOff, HoldingOn };
    enum class Pending { None, ToggleCE150, ToggleCE1600P };

    MachineController* m_controller; // not owned
    FloppyDiskManager* m_floppyManager; // not owned
    Step m_step = Step::Idle;
    Pending m_pending = Pending::None;
    int m_frameCounter = 0;

    // ~67ms hold @60Hz -- long enough for the ROM's own key-scan loop to
    // see the press.
    static constexpr int kKeyHoldFrames = 4;
    // 3s timeout @60Hz -- don't hang forever if the emulated power-down
    // path never completes.
    static constexpr int kPowerOffTimeoutFrames = 180;

    void beginToggle(Pending action);
    void performAttachToggle();
    void toggleAttachment(bool (MachineController::*isAttached)() const,
                           bool (MachineController::*attach)(),
                           void (MachineController::*detach)(),
                           void (PlotterController::*changedSignal)(bool),
                           bool (MachineController::*isOtherAttached)() const,
                           void (PlotterController::*otherChangedSignal)(bool));
};
