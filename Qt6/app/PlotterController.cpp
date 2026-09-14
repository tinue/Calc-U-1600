#include "PlotterController.hpp"
#include "MachineController.hpp"

PlotterController::PlotterController(MachineController* controller, QObject* parent)
    : QObject(parent), m_controller(controller) {}

void PlotterController::requestToggleCE150() { beginToggle(Pending::ToggleCE150); }
void PlotterController::requestToggleCE1600P() { beginToggle(Pending::ToggleCE1600P); }

void PlotterController::beginToggle(Pending action) {
    if (m_step != Step::Idle) return; // busy -- ignore, matching real hardware's "wait for it"

    m_pending = action;
    if (!m_controller->isMachinePoweredOn()) {
        // Already off: just toggle in place, no synthetic power-cycle needed.
        performAttachToggle();
        m_pending = Pending::None;
        return;
    }

    m_controller->pressKey("off");
    m_step = Step::HoldingOff;
    m_frameCounter = 0;
    emit busyChanged(true);
}

void PlotterController::onFrameTick() {
    switch (m_step) {
        case Step::Idle:
            return;
        case Step::HoldingOff:
            if (++m_frameCounter < kKeyHoldFrames) return;
            m_controller->releaseKey("off");
            m_step = Step::WaitingPoweredOff;
            m_frameCounter = 0;
            return;
        case Step::WaitingPoweredOff:
            ++m_frameCounter;
            if (m_controller->isMachinePoweredOn() && m_frameCounter < kPowerOffTimeoutFrames) return;
            performAttachToggle();
            m_controller->setOnKeyPressed(true);
            m_step = Step::HoldingOn;
            m_frameCounter = 0;
            return;
        case Step::HoldingOn:
            if (++m_frameCounter < kKeyHoldFrames) return;
            m_controller->setOnKeyPressed(false);
            m_step = Step::Idle;
            m_pending = Pending::None;
            emit busyChanged(false);
            return;
    }
}

// Shared by both plotters: toggles this one's attachment, then -- since
// Core drops the other plotter when one attaches (shared 60-pin bus) --
// reflects that in the UI too if it just got silently detached.
void PlotterController::toggleAttachment(bool (MachineController::*isAttached)() const,
                                          bool (MachineController::*attach)(),
                                          void (MachineController::*detach)(),
                                          void (PlotterController::*changedSignal)(bool),
                                          bool (MachineController::*isOtherAttached)() const,
                                          void (PlotterController::*otherChangedSignal)(bool)) {
    const bool wasOtherAttached = (m_controller->*isOtherAttached)();
    if ((m_controller->*isAttached)()) {
        (m_controller->*detach)();
    } else {
        (m_controller->*attach)();
    }
    emit (this->*changedSignal)((m_controller->*isAttached)());
    if (wasOtherAttached && !(m_controller->*isOtherAttached)()) emit (this->*otherChangedSignal)(false);
}

void PlotterController::performAttachToggle() {
    if (m_pending == Pending::ToggleCE150) {
        toggleAttachment(&MachineController::ce150Attached, &MachineController::attachCE150,
                          &MachineController::detachCE150, &PlotterController::ce150AttachedChanged,
                          &MachineController::ce1600pAttached, &PlotterController::ce1600pAttachedChanged);
    } else if (m_pending == Pending::ToggleCE1600P) {
        toggleAttachment(&MachineController::ce1600pAttached, &MachineController::attachCE1600P,
                          &MachineController::detachCE1600P, &PlotterController::ce1600pAttachedChanged,
                          &MachineController::ce150Attached, &PlotterController::ce150AttachedChanged);
    }
}

void PlotterController::syncFromMachineState() {
    emit ce150AttachedChanged(m_controller->ce150Attached());
    emit ce1600pAttachedChanged(m_controller->ce1600pAttached());
}

void PlotterController::resetOnModelSwitch() {
    m_step = Step::Idle;
    m_pending = Pending::None;
    m_frameCounter = 0;
    emit busyChanged(false);
    emit ce150AttachedChanged(false);
    emit ce1600pAttachedChanged(false);
}
