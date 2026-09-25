#include "RunControl.hpp"

#include <algorithm>

namespace debug {

RunControl::RunControl(DebugTarget& target, SourceMap& map, BreakpointTable& breakpoints)
    : m_target(target), m_map(map), m_breakpoints(breakpoints) {
    m_symbols = [this](const std::string& name, int64_t* value) {
        uint16_t v = 0;
        if (!m_map.symbolValue(name, &v)) return false;
        *value = v;
        return true;
    };
    m_bankMatch = [this](int thread, const BankKey& key, uint16_t addr) { return m_target.bankMatches(thread, key, addr); };
    m_codePeek = [this](int thread, uint16_t addr, uint8_t* value) { return m_target.peek(thread, kSpaceMain, addr, value); };
}

bool RunControl::locate(int thread, uint16_t pc, SourceLocation* out) const {
    return m_map.lookup(thread, pc, bankMatch(), out, codePeek());
}

void RunControl::pause(DebugEvent::Reason reason) {
    m_pendingPause = true;
    m_pendingReason = reason;
}

void RunControl::resume() {
    m_pendingPause = false;
    m_target.resumeFromStop();
    m_state = State::Running;
}

void RunControl::step(int thread, StepKind kind, bool line) {
    m_pendingPause = false;
    m_target.resumeFromStop();
    m_state = State::Stepping;
    m_stepThread = thread;
    m_stepKind = kind;
    m_stepRetired = m_target.retired(thread);
    m_stepSp = m_target.sp(thread);
    m_inCall = false;
    m_stepLine = line && locate(thread, m_target.pc(thread), &m_stepFrom);
}

bool RunControl::isCallKind(int thread, uint16_t pc) const {
    const disasm::Flow f = m_target.decode(thread, pc).flow;
    return f == disasm::Flow::Call || f == disasm::Flow::CondCall || f == disasm::Flow::Vector;
}

void RunControl::stopWith(DebugEvent::Reason reason, int thread, std::vector<DebugEvent>* events, std::vector<int> ids) {
    m_state = State::Paused;
    m_inCall = false;
    DebugEvent e;
    e.kind = DebugEvent::Stopped;
    e.reason = reason;
    e.thread = thread;
    e.breakpointIds = std::move(ids);
    events->push_back(std::move(e));
}

bool RunControl::handleStop(const Stop& stop, std::vector<DebugEvent>* events) {
    if (stop.kind != Stop::Breakpoint && stop.kind != Stop::Watch) return false;
    const HitDecision d = stop.kind == Stop::Breakpoint
                              ? m_breakpoints.onBreakpoint(stop.thread, m_target.pc(stop.thread), m_target, symbols())
                              : m_breakpoints.onWatch(stop.thread, stop.hit, m_target, symbols());
    for (const std::string& line : d.log) {
        DebugEvent out;
        out.kind = DebugEvent::Output;
        out.thread = stop.thread;
        out.text = line;
        events->push_back(out);
    }
    if (d.stop) {
        const DebugEvent::Reason reason = d.entry                        ? DebugEvent::Entry
                                          : stop.kind == Stop::Breakpoint ? DebugEvent::Breakpoint
                                                                          : DebugEvent::DataBreakpoint;
        stopWith(reason, stop.thread, events, d.ids);
        return true;
    }
    // Not a stop after all (condition, hit count, logpoint, other bank):
    // leave the breakpoint behind and carry on.
    if (stop.kind == Stop::Breakpoint) m_target.resumeFromStop();
    return false;
}

bool RunControl::stepDone() const {
    const int t = m_stepThread;
    if (m_target.retired(t) == m_stepRetired) return false;
    if (m_inCall) return false;
    switch (m_stepKind) {
        case StepKind::Out: {
            // The frame is gone once a Return-kind instruction left SP above
            // where it was when we started.
            if (m_target.historySize(t) == 0) return false;
            const HistoryEntry last = m_target.history(t, 0);
            if (last.interrupt || last.len == 0) return false;
            const disasm::Decoded dec = m_target.decode(t, last);
            return (dec.flow == disasm::Flow::Return || dec.flow == disasm::Flow::CondReturn) && m_target.sp(t) > m_stepSp;
        }
        case StepKind::Instruction:
            return true;
        case StepKind::In:
        case StepKind::Over: {
            if (!m_stepLine) return true;
            SourceLocation now;
            if (!locate(t, m_target.pc(t), &now)) return true; // left the source: show the disassembly
            return now.file != m_stepFrom.file || now.line != m_stepFrom.line;
        }
    }
    return true;
}

std::vector<DebugEvent> RunControl::slice(uint64_t budget, uint64_t maxSteps) {
    std::vector<DebugEvent> events;
    if (m_pendingPause) {
        m_pendingPause = false;
        stopWith(m_pendingReason, m_state == State::Stepping ? m_stepThread : m_target.busOwner(), &events);
        return events;
    }
    if (m_state == State::Paused) return events;

    if (m_state == State::Running) {
        uint64_t remaining = budget;
        while (remaining > 0) {
            const Stop s = m_target.runMachine(remaining);
            remaining -= std::min(s.cycles, remaining);
            if (s.kind == Stop::None) break;
            if (handleStop(s, &events)) return events;
            if (s.cycles == 0 && s.kind != Stop::Breakpoint) break; // no progress; try next frame
        }
        return events;
    }

    // Stepping
    const int t = m_stepThread;
    for (uint64_t i = 0; i < maxSteps; i++) {
        // Over: a Call-kind instruction the stepped CPU is about to execute
        // runs to its return as one step.
        if (m_stepKind == StepKind::Over && !m_inCall && m_target.busOwner() == t && !m_target.halted(t) &&
            isCallKind(t, m_target.pc(t))) {
            m_inCall = true;
            m_callSp = m_target.sp(t);
        }
        const uint32_t before = m_target.retired(t);
        const Stop s = m_target.stepMachine();
        if (s.kind != Stop::None && handleStop(s, &events)) return events;
        if (m_target.retired(t) == before) continue;
        // Back at the caller's stack depth: the call returned (or, a
        // conditional one, was never taken). A taken call lowers SP first.
        if (m_inCall && m_target.sp(t) >= m_callSp) m_inCall = false;
        if (stepDone()) {
            stopWith(DebugEvent::Step, t, &events);
            return events;
        }
    }
    return events;
}

} // namespace debug
