#pragma once

#include <cstdint>

// Why a machine's step()/runCycles() stopped for the debugger, and which
// CPU: 1 = the first CPU (PC-1500 LH5801, PC-1600 Z-80), 2 = the PC-1600's
// LH5803 -- the debugger's thread ids.
struct DebugStop {
    enum Kind : uint8_t { None, Breakpoint, Watch };
    Kind kind = None;
    uint8_t cpu = 0;
};

// Holds the first stop until the debugger consumes it.
class DebugStopLatch {
public:
    void latch(DebugStop::Kind kind, int cpu) {
        if (m_stop.kind == DebugStop::None) m_stop = {kind, static_cast<uint8_t>(cpu)};
    }
    DebugStop consume() {
        const DebugStop s = m_stop;
        m_stop = {};
        return s;
    }
    DebugStop::Kind pending() const { return m_stop.kind; }
    void clear() { m_stop = {}; }

private:
    DebugStop m_stop;
};
