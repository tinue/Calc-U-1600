#pragma once
#include <QObject>
#include <functional>

class MachineController;

// Owns the attach/detach power cycle for the 60-pin peripherals -- the
// CE-150/CE-1600P plotters and the CE-158 interface: real hardware requires the calculator to be powered off, connected, then
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

    void requestToggleCE150() { beginToggle(/*isCE150=*/true); }
    void requestToggleCE1600P() { beginToggle(/*isCE150=*/false); }
    // The CE-158 shares the bus with the CE-150 -- no mutual exclusion.
    void requestToggleCE158();

    // Runs a toggle's power cycle: called with the attach/detach step, must
    // run it inside the flat-out OFF/ON cycle. Unset: the step runs directly.
    void setPowerCycleRunner(std::function<void(const std::function<void()>&)> runner) {
        m_powerCycleRunner = std::move(runner);
    }

    // Re-emits ce150AttachedChanged/ce1600pAttachedChanged from whatever
    // MachineController currently reports. Used after anything that changed
    // the attach state behind this class's back -- a machine rebuild
    // (switchModel()), or a preset attaching a plotter directly on the Core
    // machine -- so the GUI shows what is really attached.
    void syncFromMachineState();

signals:
    void ce150AttachedChanged(bool attached);
    void ce1600pAttachedChanged(bool attached);
    void ce158AttachedChanged(bool attached);

private:
    MachineController* m_controller; // not owned
    std::function<void(const std::function<void()>&)> m_powerCycleRunner;

    void beginToggle(bool isCE150);
    void toggleAttachment(bool isCE150);
};
