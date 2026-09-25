#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BreakpointTable.hpp"
#include "DebugTarget.hpp"
#include "SourceMap.hpp"

// ── Run control ──────────────────────────────────────────────────────────
//
// The debugger's execution state machine: paused, running (free run with
// breakpoints and watches), or one of the step modes. The app calls
// slice() once per emulation frame with that frame's cycle budget; every
// mode does a bounded amount of work per slice, so the GUI stays live
// while a long step over runs. What happened comes back as events.
//
// Stepping granularity: "instruction" steps one instruction of the chosen
// CPU; "line" steps until that CPU reaches a different source line (or
// code without source). Over steps across Call/Vector instructions by
// running until the stack is back where it was -- which also covers a
// VMJ whose inline parameters move the return address. Out runs until a
// Return-kind instruction pops the current frame.

namespace debug {

struct DebugEvent {
    enum Kind : uint8_t { Stopped, Output };
    Kind kind = Stopped;
    enum Reason : uint8_t { Breakpoint, DataBreakpoint, Step, Pause, Entry };
    Reason reason = Pause;
    int thread = 0;
    std::vector<int> breakpointIds;
    std::string text; ///< Output: the logpoint / message line; Stopped: a description
};

class RunControl {
public:
    enum class State : uint8_t { Paused, Running, Stepping };
    enum class StepKind : uint8_t { Instruction, In, Over, Out };

    RunControl(DebugTarget& target, SourceMap& map, BreakpointTable& breakpoints);
    RunControl(const RunControl&) = delete; // its lookups capture `this`
    RunControl& operator=(const RunControl&) = delete;

    bool paused() const { return m_state == State::Paused; }

    /// Stops now (reason Pause, or Entry after a reset); the event comes
    /// from the next slice().
    void pause(DebugEvent::Reason reason = DebugEvent::Pause);
    void resume();
    /// `line`: source-line granularity when the CPU's PC has source,
    /// instruction granularity otherwise.
    void step(int thread, StepKind kind, bool line);

    /// Does one frame's worth of work. `budget` is the machine-cycle budget
    /// of a free run; stepping does at most `maxSteps` machine steps.
    std::vector<DebugEvent> slice(uint64_t budget, uint64_t maxSteps = 20000);

    /// Symbols for conditions and logpoints (the source map's).
    const BreakpointTable::SymbolLookup& symbols() const { return m_symbols; }
    /// The target's bank predicate, for source lookups.
    const BankMatch& bankMatch() const { return m_bankMatch; }
    /// Side-effect-free code reads, for source lookups that check bytes.
    const CodePeek& codePeek() const { return m_codePeek; }
    /// Source location of `thread`'s PC right now (false: no source).
    bool locate(int thread, uint16_t pc, SourceLocation* out) const;

private:
    /// Handles a Stop from the target. True if execution must halt (an
    /// event was queued).
    bool handleStop(const Stop& stop, std::vector<DebugEvent>* events);
    void stopWith(DebugEvent::Reason reason, int thread, std::vector<DebugEvent>* events,
                  std::vector<int> ids = {});
    bool isCallKind(int thread, uint16_t pc) const;
    bool stepDone() const;

    DebugTarget& m_target;
    SourceMap& m_map;
    BreakpointTable& m_breakpoints;
    // Built once: every source lookup and breakpoint hit uses them.
    BreakpointTable::SymbolLookup m_symbols;
    BankMatch m_bankMatch;
    CodePeek m_codePeek;
    State m_state = State::Paused;
    bool m_pendingPause = false;
    DebugEvent::Reason m_pendingReason = DebugEvent::Pause;

    // Step state
    int m_stepThread = 1;
    StepKind m_stepKind = StepKind::Instruction;
    bool m_stepLine = false;
    SourceLocation m_stepFrom;   // the line a line step started on
    uint16_t m_stepSp = 0;       // SP at the start (Over/Out) or of the call being stepped over
    uint32_t m_stepRetired = 0;  // instructions retired at the start
    bool m_inCall = false;       // Over: running a Call-kind instruction to its return
    uint16_t m_callSp = 0;
};

} // namespace debug
