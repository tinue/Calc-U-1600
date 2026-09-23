#include "PlotterController.hpp"
#include "MachineController.hpp"

PlotterController::PlotterController(MachineController* controller, QObject* parent)
    : QObject(parent), m_controller(controller) {}

void PlotterController::beginToggle(bool isCE150) {
    auto change = [this, isCE150] { toggleAttachment(isCE150); };
    if (m_powerCycleRunner) m_powerCycleRunner(change);
    else change();
}

// Toggles this plotter's attachment, then -- since Core drops the other
// plotter when one attaches (shared 60-pin bus) -- reflects that in the UI
// too if it just got silently detached.
void PlotterController::toggleAttachment(bool isCE150) {
    const bool wasOtherAttached = isCE150 ? m_controller->ce1600pAttached() : m_controller->ce150Attached();

    if (isCE150) {
        if (m_controller->ce150Attached()) m_controller->detachCE150();
        else m_controller->attachCE150();
    } else {
        if (m_controller->ce1600pAttached()) m_controller->detachCE1600P();
        else m_controller->attachCE1600P();
    }

    if (isCE150) {
        emit ce150AttachedChanged(m_controller->ce150Attached());
        if (wasOtherAttached && !m_controller->ce1600pAttached()) emit ce1600pAttachedChanged(false);
    } else {
        emit ce1600pAttachedChanged(m_controller->ce1600pAttached());
        if (wasOtherAttached && !m_controller->ce150Attached()) emit ce150AttachedChanged(false);
    }
}

void PlotterController::requestToggleCE158() {
    auto change = [this] {
        if (m_controller->ce158Attached()) m_controller->detachCE158();
        else m_controller->attachCE158();
        emit ce158AttachedChanged(m_controller->ce158Attached());
    };
    if (m_powerCycleRunner) m_powerCycleRunner(change);
    else change();
}

void PlotterController::syncFromMachineState() {
    emit ce150AttachedChanged(m_controller->ce150Attached());
    emit ce1600pAttachedChanged(m_controller->ce1600pAttached());
    emit ce158AttachedChanged(m_controller->ce158Attached());
}
