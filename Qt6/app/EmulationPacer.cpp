#include "EmulationPacer.hpp"

#include <QTimer>
#include <utility>

#include "AudioOutput.hpp"
#include "MachineController.hpp"

namespace {
// ~60 Hz.
constexpr int kFrameIntervalMs = 16;
} // namespace

EmulationPacer::EmulationPacer(MachineController* controller, AudioOutput* audio,
                               std::function<void()> refreshViews, QObject* parent)
    : QObject(parent), m_controller(controller), m_audio(audio), m_refreshViews(std::move(refreshViews)) {
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
    const bool turbo = m_controller->app().tick();
    m_audio->pump(*m_controller, /*discard=*/turbo);
    m_refreshViews();
}

void EmulationPacer::runFor(double seconds) {
    shared::runEmulatedSeconds(m_controller->session(), seconds);
    m_controller->app().frameDuties();
    m_controller->discardAudio(); // not paced in real time -- nothing to play
    m_refreshViews();
}

bool EmulationPacer::runUntilPasteDone(double capSeconds) {
    const bool done = shared::runUntilPasteDone(m_controller->session(), capSeconds);
    m_controller->app().frameDuties();
    m_controller->discardAudio();
    m_refreshViews();
    return done;
}
