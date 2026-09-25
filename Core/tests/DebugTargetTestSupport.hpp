#pragma once

#include <cstdint>
#include <functional>

#include "../Debug/DebugTarget.hpp"

// Convenience drivers over DebugTarget's primitives for the tests. The app
// drives targets through RunControl, which manages resumes itself.

namespace debugtest {

/// Free run for up to `budget` machine cycles, resuming past a breakpoint
/// the CPUs sit on.
inline debug::Stop runFrom(debug::DebugTarget& target, uint64_t budget) {
    target.resumeFromStop();
    return target.runMachine(budget);
}

/// Steps until `done` returns true after a step, a breakpoint or watch
/// stops it, or `maxSteps` machine steps have run (Stop::Budget).
inline debug::Stop runUntil(debug::DebugTarget& target, uint64_t maxSteps, const std::function<bool()>& done) {
    target.resumeFromStop();
    for (uint64_t i = 0; i < maxSteps; i++) {
        debug::Stop s = target.stepMachine();
        if (s.kind != debug::Stop::None) return s;
        if (done()) return {};
    }
    debug::Stop s;
    s.kind = debug::Stop::Budget;
    return s;
}

/// Steps until `thread` has retired one instruction (the other CPU of a
/// PC-1600 runs meanwhile if it owns the bus).
inline debug::Stop stepInstruction(debug::DebugTarget& target, int thread, uint64_t maxSteps) {
    const uint32_t before = target.retired(thread);
    return runUntil(target, maxSteps, [&target, thread, before] { return target.retired(thread) != before; });
}

} // namespace debugtest
