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
    const bool hadCE158 = m_controller->ce158Attached();

    QString error;
    if (isCE150) {
        if (m_controller->ce150Attached()) m_controller->detachCE150();
        else if (!m_controller->attachCE150(&error)) emit attachFailed(tr("CE-150"), error);
    } else {
        if (m_controller->ce1600pAttached()) m_controller->detachCE1600P();
        else if (!m_controller->attachCE1600P(&error)) emit attachFailed(tr("CE-1600P"), error);
    }

    if (isCE150) {
        emit ce150AttachedChanged(m_controller->ce150Attached());
        if (wasOtherAttached && !m_controller->ce1600pAttached()) emit ce1600pAttachedChanged(false);
    } else {
        emit ce1600pAttachedChanged(m_controller->ce1600pAttached());
        if (wasOtherAttached && !m_controller->ce150Attached()) emit ce150AttachedChanged(false);
    }
    // Attaching the CE-1600P drops a CE-158 (PC-1600).
    if (hadCE158 && !m_controller->ce158Attached()) emit ce158AttachedChanged(false);
}

void PlotterController::requestToggleCE158() {
    auto change = [this] {
        const bool hadCE1600P = m_controller->ce1600pAttached();
        QString error;
        if (m_controller->ce158Attached()) m_controller->detachCE158();
        else if (!m_controller->attachCE158(&error)) emit attachFailed(tr("CE-158"), error);
        emit ce158AttachedChanged(m_controller->ce158Attached());
        // A PC-1600 drops the CE-1600P to make room for the CE-158.
        if (hadCE1600P && !m_controller->ce1600pAttached()) emit ce1600pAttachedChanged(false);
    };
    if (m_powerCycleRunner) m_powerCycleRunner(change);
    else change();
}

void PlotterController::syncFromMachineState() {
    emit ce150AttachedChanged(m_controller->ce150Attached());
    emit ce1600pAttachedChanged(m_controller->ce1600pAttached());
    emit ce158AttachedChanged(m_controller->ce158Attached());
}
