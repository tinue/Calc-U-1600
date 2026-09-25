#pragma once
#include <QObject>
#include <QString>
#include <functional>

class MachineController;

// Owns the attach/detach power cycle for the 60-pin peripherals -- the
// CE-150/CE-1600P plotters and the CE-158 interface: real hardware requires the calculator to be powered off, connected, then
// powered back on -- not a live hot-plug. The cycle itself (OFF, power-down,
// the instantaneous Core attach/detach call, ON, boot) is run flat out and
// synchronously by the runner MainWindow installs (see
// MachineController::powerCycleAround()); this class supplies the attach/
// detach step and reports the resulting state. No machine object is rebuilt
// (unlike MachineController::switchModel()). Which devices can coexist is
// Core's rule alone (attaching one may detach another); this class never
// tracks it, it just re-reads every attach state afterwards.
class PlotterController : public QObject {
    Q_OBJECT
public:
    explicit PlotterController(MachineController* controller, QObject* parent = nullptr);

    void requestToggleCE150() { beginToggle(/*isCE150=*/true); }
    void requestToggleCE1600P() { beginToggle(/*isCE150=*/false); }
    void requestToggleCE158();

    // Runs a toggle's power cycle: called with the attach/detach step, must
    // run it inside the flat-out OFF/ON cycle. Unset: the step runs directly.
    void setPowerCycleRunner(std::function<void(const std::function<void()>&)> runner) {
        m_powerCycleRunner = std::move(runner);
    }

    // Emits attachStateChanged(). Used after anything that changed the
    // attach state behind this class's back -- a machine rebuild
    // (switchModel()), or a preset attaching a device directly on the Core
    // machine -- so the GUI shows what is really attached.
    void syncFromMachineState() { emit attachStateChanged(); }

signals:
    // Some CE-150/CE-1600P/CE-158 attach state may have changed: read all
    // three back from MachineController.
    void attachStateChanged();
    // An attach the user asked for failed (`reason`: e.g. a missing ROM);
    // the device stays detached.
    void attachFailed(const QString& device, const QString& reason);

private:
    MachineController* m_controller; // not owned
    std::function<void(const std::function<void()>&)> m_powerCycleRunner;

    void beginToggle(bool isCE150);
    // Runs `change` inside the power cycle (if a runner is set), then
    // reports the resulting attach state.
    void runToggle(const std::function<void()>& change);
};
