#include "PlotterController.hpp"
#include "MachineController.hpp"

PlotterController::PlotterController(MachineController* controller, QObject* parent)
    : QObject(parent), m_controller(controller) {}

void PlotterController::runToggle(const std::function<void()>& change) {
    if (m_powerCycleRunner) m_powerCycleRunner(change);
    else change();
    syncFromMachineState();
}

void PlotterController::beginToggle(bool isCE150) {
    runToggle([this, isCE150] {
        QString error;
        if (isCE150) {
            if (m_controller->ce150Attached()) m_controller->detachCE150();
            else if (!m_controller->attachCE150(&error)) emit attachFailed(tr("CE-150"), error);
        } else {
            if (m_controller->ce1600pAttached()) m_controller->detachCE1600P();
            else if (!m_controller->attachCE1600P(&error)) emit attachFailed(tr("CE-1600P"), error);
        }
    });
}

void PlotterController::requestToggleCE158() {
    runToggle([this] {
        QString error;
        if (m_controller->ce158Attached()) m_controller->detachCE158();
        else if (!m_controller->attachCE158(&error)) emit attachFailed(tr("CE-158"), error);
    });
}
