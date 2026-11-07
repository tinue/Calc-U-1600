#include "EmulationPacer.hpp"

#include <QTimer>
#include <utility>

#include "MachineController.hpp"

namespace {
// ~60 Hz.
constexpr int kFrameIntervalMs = 16;
} // namespace

EmulationPacer::EmulationPacer(MachineController* controller, std::function<void()> refreshViews, QObject* parent)
    : QObject(parent), m_controller(controller), m_refreshViews(std::move(refreshViews)) {
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &EmulationPacer::onTick);
    restart();
    resume();
}

void EmulationPacer::suspend() { m_timer->stop(); }

void EmulationPacer::resume() {
    if (!m_frozen) m_timer->start(kFrameIntervalMs);
}

void EmulationPacer::restart() {
    m_controller->seedClockFromHost();
    m_controller->app().pacer().restart();
}

void EmulationPacer::setTurbo(bool active) { m_controller->app().pacer().setTurbo(active); }

void EmulationPacer::setFrozen(bool frozen) {
    m_frozen = frozen;
    if (frozen) {
        m_timer->stop();
        return;
    }
    restart();
    resume();
}

void EmulationPacer::onTick() {
    m_controller->app().tick();
    m_refreshViews();
}

void EmulationPacer::runFor(double seconds) {
    m_controller->app().runFor(seconds);
    m_refreshViews();
}

bool EmulationPacer::runUntilPasteDone(double capSeconds) {
    const bool done = m_controller->app().runUntilPasteDone(capSeconds);
    m_refreshViews();
    return done;
}
