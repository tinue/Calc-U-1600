#pragma once
#include <cstdint>
#include <functional>

namespace shared {

class MachineSession;

// ── Pacer: how much emulated time a frame tick runs ──────────────────────
//
// The frontend calls tick() about every 16 ms (a QTimer, a browser's
// requestAnimationFrame or a worker interval). Paced, a tick runs exactly
// the wall-clock time since start: cycles are credited against an absolute
// deadline (start + elapsed x clockHz), never re-measured from the last
// tick, so timer jitter and oversleep can't build up into a TIME lag
// (docs/background/Decisions.md, "Absolute-deadline emulation pacing").
// A stall longer than kMaxCatchUpSeconds (a modal dialog, a window drag, a
// background tab) is dropped rather than caught up in one burst. In turbo
// (the LCD held down) a tick instead runs as many emulated frames as fit
// in kTurboSliceSeconds of host time.
class Pacer {
public:
    // Monotonic host time in nanoseconds; steady_clock unless a test
    // injects its own.
    using Clock = std::function<std::int64_t()>;
    static std::int64_t steadyNowNs();

    static constexpr double kMaxCatchUpSeconds = 0.1;
    static constexpr double kTurboSliceSeconds = 0.014;

    explicit Pacer(Clock clock = steadyNowNs) : m_clock(std::move(clock)) { restart(); }

    // Rebase pacing on "now", so the next tick doesn't catch up on time
    // spent elsewhere (a flat-out load, a suspended timer).
    void restart();

    // Cycles a paced tick at `nowNs` should run for a machine at `clockHz`
    // (a change of clock rate rebases). Advances the deadline.
    std::uint64_t cyclesDue(std::int64_t nowNs, double clockHz);

    void setTurbo(bool on) { m_turbo = on; }
    bool turbo() const { return m_turbo; }

    // One frame tick on `session`: paced, or turbo. Returns true if it ran
    // turbo (the audio produced is then not real-time and is discarded by
    // the caller).
    bool tick(MachineSession& session);

private:
    Clock m_clock;
    std::int64_t m_startNs = 0;
    double m_clockHz = 0;
    std::uint64_t m_granted = 0;  // cycles credited since m_startNs
    bool m_turbo = false;
};

// Runs `seconds` of emulated time in 60 Hz frame slices (scripted runs:
// emulated time only, no pacing).
void runEmulatedSeconds(MachineSession& session, double seconds);
// Runs 60 Hz frames until a paste or typed command has been fully typed;
// false if still typing after `capSeconds` of emulated time.
bool runUntilPasteDone(MachineSession& session, double capSeconds);

} // namespace shared
