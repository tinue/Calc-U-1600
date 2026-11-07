#include "Shared/Pacer.hpp"

#include <algorithm>
#include <chrono>

#include "Shared/MachineSession.hpp"

namespace shared {

std::int64_t Pacer::steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Pacer::restart() {
    m_startNs = m_clock();
    m_granted = 0;
}

std::uint64_t Pacer::cyclesDue(std::int64_t nowNs, double clockHz) {
    if (clockHz != m_clockHz) {
        m_clockHz = clockHz;
        m_startNs = nowNs;
        m_granted = 0;
        return 0;
    }
    const double elapsed = static_cast<double>(nowNs - m_startNs) * 1e-9;
    const double target = elapsed * clockHz;
    if (target <= static_cast<double>(m_granted)) return 0;
    auto due = static_cast<std::uint64_t>(target) - m_granted;
    const auto cap = static_cast<std::uint64_t>(clockHz * kMaxCatchUpSeconds);
    if (due > cap) {
        // A stall: run one capped tick and move the deadline so the rest
        // is dropped, not caught up.
        due = cap;
        m_startNs = nowNs;
        m_granted = 0;
        return due;
    }
    m_granted += due;
    return due;
}

bool Pacer::tick(MachineSession& session) {
    const double clockHz = session.clockHz();
    if (!m_turbo) {
        session.advance(cyclesDue(m_clock(), clockHz));
        return false;
    }
    // Press-and-hold on the LCD: run unthrottled, as many emulated frames
    // as the host produces within this tick's slice. The display still only
    // repaints once per tick, so this reads as a fast-forward.
    const auto perFrame = static_cast<std::uint64_t>(clockHz / 60.0);
    const std::int64_t deadline = m_clock() + static_cast<std::int64_t>(kTurboSliceSeconds * 1e9);
    do {
        session.advance(perFrame);
    } while (m_clock() < deadline);
    restart();
    return true;
}

void runEmulatedSeconds(MachineSession& session, double seconds) {
    const double clockHz = session.clockHz();
    const auto perFrame = static_cast<std::uint64_t>(clockHz / 60.0);
    auto remaining = static_cast<std::uint64_t>(seconds * clockHz);
    while (remaining > 0) {
        const std::uint64_t slice = std::min(remaining, perFrame);
        session.advance(slice);
        remaining -= slice;
    }
}

bool runUntilPasteDone(MachineSession& session, double capSeconds) {
    const auto perFrame = static_cast<std::uint64_t>(session.clockHz() / 60.0);
    const int maxFrames = static_cast<int>(capSeconds * 60.0);
    for (int i = 0; i < maxFrames && session.pasteActive(); ++i) session.advance(perFrame);
    return !session.pasteActive();
}

} // namespace shared
