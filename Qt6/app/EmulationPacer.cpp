#include "EmulationPacer.hpp"

#include <QCoreApplication>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>

#include "AudioOutput.hpp"
#include "MachineController.hpp"

namespace {
// ~60 Hz. Shared by the frame timer's own period and by the turbo
// fast-forward budget below, so the two stay in lockstep if this changes.
constexpr int kFrameIntervalMs = 16;
// Most emulated time a single non-turbo tick may run -- see onTick().
constexpr double kMaxTickSeconds = 0.1;
} // namespace

EmulationPacer::EmulationPacer(MachineController* controller, AudioOutput* audio,
                               std::function<void()> refreshViews, QObject* parent)
    : QObject(parent), m_controller(controller), m_audio(audio), m_refreshViews(std::move(refreshViews)) {
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &EmulationPacer::onTick);
    resume();
}

void EmulationPacer::suspend() { m_timer->stop(); }

void EmulationPacer::resume() {
    restartPacing();
    if (!m_frozen) m_timer->start(kFrameIntervalMs);
}

void EmulationPacer::restartPacing() {
    m_paceClock.restart();
    m_cycleCarry = 0.0;
}

void EmulationPacer::setFrozen(bool frozen) {
    m_frozen = frozen;
    if (frozen) m_timer->stop();
    else resume();
}

void EmulationPacer::onTick() {
    const double clockHz = m_controller->clockHz();
    const std::uint64_t cyclesPerFrame = static_cast<std::uint64_t>(clockHz / 60.0);
    if (m_turbo) {
        // Press-and-hold on the LCD: run unthrottled, i.e. as many emulated
        // cycles as the host can produce within this tick's wall-clock
        // budget, instead of the usual real-time-paced amount. The display
        // still only repaints once per tick (below), so this reads as a
        // fast-forward rather than a smoother/faster-refreshing picture.
        // processEvents() is pumped between bursts so the mouse-release
        // event that ends turbo (and any paint/close events) isn't starved
        // for the whole budget.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kFrameIntervalMs - 2);
        do {
            m_controller->advance(cyclesPerFrame);
            QCoreApplication::processEvents();
        } while (m_turbo && std::chrono::steady_clock::now() < deadline);
        restartPacing();
    } else if (m_controller->resyncClockIfSeeded()) {
        // First tick after a flat-out run that set the clock: re-seeded
        // just now, so pace from here (see resyncClockIfSeeded()).
        restartPacing();
    } else {
        // Run exactly the wall-clock time since the last tick, carrying the
        // fractional cycle, rather than a fixed clockHz/60 per 16 ms tick
        // (which ran ~4% fast and jittered with the timer). Real-time
        // emulated time is what keeps the buzzer audio from under- or
        // overrunning the sound device. Capped so a stall (modal dialog,
        // window drag) doesn't turn into a catch-up burst.
        const double elapsedSeconds = static_cast<double>(m_paceClock.nsecsElapsed()) * 1e-9;
        m_paceClock.restart();
        const double cycles = std::min(m_cycleCarry + elapsedSeconds * clockHz, clockHz * kMaxTickSeconds);
        const auto whole = static_cast<std::uint64_t>(cycles);
        m_cycleCarry = cycles - static_cast<double>(whole);
        m_controller->advance(whole);
    }
    m_audio->pump(*m_controller, /*discard=*/m_turbo);
    m_refreshViews();
}

void EmulationPacer::runFor(double seconds) {
    const double clockHz = m_controller->clockHz();
    const auto perFrame = static_cast<std::uint64_t>(clockHz / 60.0);
    auto remaining = static_cast<std::uint64_t>(seconds * clockHz);
    while (remaining > 0) {
        const std::uint64_t slice = std::min(remaining, perFrame);
        m_controller->advance(slice);
        remaining -= slice;
    }
    m_controller->discardAudio(); // not paced in real time -- nothing to play
    m_refreshViews();
}

bool EmulationPacer::runUntilPasteDone(double capSeconds) {
    // advance() pumps the paste queue; refresh the views once at the end.
    const auto perFrame = static_cast<std::uint64_t>(m_controller->clockHz() / 60.0);
    const int maxFrames = static_cast<int>(capSeconds * 60.0);
    for (int i = 0; i < maxFrames && m_controller->pasteActive(); ++i) m_controller->advance(perFrame);
    m_controller->discardAudio();
    m_refreshViews();
    return !m_controller->pasteActive();
}
