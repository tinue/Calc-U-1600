#pragma once
#include <QObject>
#include <functional>

class MachineController;

// Owns the plotter attach/detach power cycle: real CE-150/CE-1600P
// hardware requires the calculator to be powered off, connected, then
// powered back on -- not a live hot-plug. The cycle itself (OFF, power-down,
// the instantaneous Core attach/detach call, ON, boot) is run flat out and
// synchronously by the runner MainWindow installs (see
// MachineController::powerCycleAround()); this class supplies the attach/
// detach step and reports the resulting state. No machine object is rebuilt
// (unlike MachineController::switchModel()).
class PlotterController : public QObject {
    Q_OBJECT
public:
    explicit PlotterController(MachineController* controller, QObject* parent = nullptr);

    void requestToggleCE150();
    void requestToggleCE1600P();

    // Runs a toggle's power cycle: called with the attach/detach step, must
    // run it inside the flat-out OFF/ON cycle. Unset: the step runs directly.
    void setPowerCycleRunner(std::function<void(const std::function<void()>&)> runner) {
        m_powerCycleRunner = std::move(runner);
    }

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

private:
    enum class Pending { None, ToggleCE150, ToggleCE1600P };

    MachineController* m_controller; // not owned
    Pending m_pending = Pending::None;
    std::function<void(const std::function<void()>&)> m_powerCycleRunner;

    void beginToggle(Pending action);
    void performAttachToggle();
    void toggleAttachment(bool (MachineController::*isAttached)() const,
                           bool (MachineController::*attach)(),
                           void (MachineController::*detach)(),
                           void (PlotterController::*changedSignal)(bool),
                           bool (MachineController::*isOtherAttached)() const,
                           void (PlotterController::*otherChangedSignal)(bool));
};
