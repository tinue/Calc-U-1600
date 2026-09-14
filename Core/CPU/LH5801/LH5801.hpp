#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "../../TraceTypes.hpp"

// ── Bus interface ────────────────────────────────────────────────────────
//
// The LH5801 addresses two independent 64KB banks, ME0 and ME1, selected by
// the instruction's own addressing-mode encoding ((addr) vs #(addr)) — not by
// a CPU-side bank register. PC1500Memory (and later PC1600's LH5803 wiring)
// implements this interface; the CPU core itself has no opinion on what's
// behind either bank.
class LH5801Bus {
public:
    virtual ~LH5801Bus() = default;

    virtual uint8_t readME0(uint16_t addr) = 0;
    virtual void    writeME0(uint16_t addr, uint8_t value) = 0;
    // ME1 defaults to aliasing ME0 — the conservative choice for a bus with
    // no real ME1 wiring of its own (never a hard fault on an incidental
    // #(addr) access). Every concrete bus gets this for free; override only
    // once a machine gives ME1 real, distinct semantics (e.g. PC-1600's
    // LH5803 wiring in a later phase).
    virtual uint8_t readME1(uint16_t addr) { return readME0(addr); }
    virtual void    writeME1(uint16_t addr, uint8_t value) { writeME0(addr, value); }

    // I/O port (ITA/ATP) and LCD enable (SDP/RDP). Default no-ops so a bus
    // that doesn't care about these (e.g. a bare-CPU unit test) needn't
    // implement them.
    virtual uint8_t readInputPort() { return 0xFF; }
    virtual void    writeOutputPort(uint8_t) {}
    virtual void    setDisplayEnabled(bool) {}
};

// ── LH5801 CPU core ──────────────────────────────────────────────────────
//
// Sharp LH5801 8-bit CPU, as used standalone in the PC-1500/PC-1500A and as
// the base for the PC-1600's LH5803 compatibility co-processor. Full
// documented instruction set.
//
// Carries its own instruction-trace ring buffer and breakpoint list — a
// core-level, GUI-independent feature exercised directly by headless
// tests.
class LH5801 {
public:
    explicit LH5801(LH5801Bus& bus);

    /// Cold reset: PU/PV/DISP/T/TM cleared, P loaded from the 16-bit
    /// big-endian reset vector at ME0 0xFFFE/0xFFFF (confirmed against the
    /// real PC-1500_A04.ROM image — see Core/tests/lh5801_tests.cpp).
    void reset();

    /// Execute one instruction at the current P.
    /// Returns the cycle count consumed, or 0 if execution did not advance
    /// because a breakpoint at the current PC was just hit (see
    /// consumeBreakpointHit()) or the CPU is halted (HLT) with no pending
    /// interrupt.
    int step();

    // ── Register access (debug / tests / trace) ──────────────────────────
    uint8_t  a()  const { return A; }
    void     setA(uint8_t v) { A = v; }
    uint8_t  xl() const { return XL; }
    uint8_t  xh() const { return XH; }
    uint8_t  yl() const { return YL; }
    uint8_t  yh() const { return YH; }
    uint8_t  ul() const { return UL; }
    uint8_t  uh() const { return UH; }
    void     setXL(uint8_t v) { XL = v; }
    void     setXH(uint8_t v) { XH = v; }
    void     setYL(uint8_t v) { YL = v; }
    void     setYH(uint8_t v) { YH = v; }
    void     setUL(uint8_t v) { UL = v; }
    void     setUH(uint8_t v) { UH = v; }
    uint16_t x() const { return (uint16_t(XH) << 8) | XL; }
    uint16_t y() const { return (uint16_t(YH) << 8) | YL; }
    uint16_t u() const { return (uint16_t(UH) << 8) | UL; }
    void     setX(uint16_t v) { XH = uint8_t(v >> 8); XL = uint8_t(v); }
    void     setY(uint16_t v) { YH = uint8_t(v >> 8); YL = uint8_t(v); }
    void     setU(uint16_t v) { UH = uint8_t(v >> 8); UL = uint8_t(v); }

    uint16_t pc() const { return P; }
    void     setPC(uint16_t v) { P = v; }
    uint16_t sp() const { return S; }
    void     setSP(uint16_t v) { S = v; }

    uint8_t  statusReg() const { return T; }
    void     setStatusReg(uint8_t v) { T = v & 0x1F; }
    bool     flagC() const { return (T & 0x01) != 0; }
    bool     flagIE() const { return (T & 0x02) != 0; }
    bool     flagZ() const { return (T & 0x04) != 0; }
    bool     flagV() const { return (T & 0x08) != 0; }
    bool     flagH() const { return (T & 0x10) != 0; }
    void     setFlagC(bool v) { setFlagBit(0x01, v); }
    void     setFlagZ(bool v) { setFlagBit(0x04, v); }

    /// Elapsed-cycle units credited toward the timer's tick divisor
    /// (LH5801TimerTable.hpp's kCyclesPerTick) per step() call made while
    /// halted -- exactly 1, since a halted CPU still lets the timer's LFSR
    /// accumulate elapsed cycles at the normal rate; using kCyclesPerTick
    /// itself here would tick the timer a full LFSR step per halted
    /// step() call, running it 8x too fast. Public so callers pacing a run
    /// budget (PC1500Machine::runCycles) can account for idle-but-ticking
    /// time consistently with step()'s own internal accounting.
    static constexpr int kHaltTickCycles = 1;

    bool     pu() const { return PU; }
    bool     pv() const { return PV; }
    void     setPU(bool v) { PU = v; }
    void     setPV(bool v) { PV = v; }
    bool     displayOn() const { return DISP; }
    uint16_t timer() const { return TM; }
    /// Debug/test/state-load access to AM0/AM1's own load path (resyncs
    /// the internal LFSR step position too -- see the .cpp comment on the
    /// private overload this forwards to).
    void setTimer(uint16_t v);
    bool     halted() const { return m_halted; }

    /// True once the OFF instruction (0xFD 0x4C, BF flip-flop reset) has
    /// run -- real hardware's own genuine power-down, distinct from HLT:
    /// the CPU clock itself stops, so unlike HLT (which keeps ticking TM
    /// and can be woken by an ordinary timer interrupt) nothing but
    /// powerOn() -- the ON key's BFI pin -- clears this. See step()'s own
    /// comment on the powered-off branch and PC1500Machine::setOnKeyPressed().
    bool     poweredOff() const { return m_poweredOff; }

    /// The ON key's BFI-pin wake from a genuine OFF power-down: real
    /// hardware physically restores power and cold-resets the CPU (the
    /// ROM's own reset vector then does cold-vs-warm detection from a RAM
    /// signature) -- unlike wakeFromHalt(), which just resumes fetch/
    /// execute right where a HLT left off, powering back on is a real
    /// reset(). reset() itself clears m_poweredOff, so this always lands
    /// running.
    void powerOn() { reset(); }

    /// Request a maskable interrupt (the LH5801's timer/general-IRQ class;
    /// its separate, always-unmasked NMI line at vector 0xFFFC is not
    /// modeled by this core -- see requestMaskableInterrupt()'s .cpp
    /// comment). Both waking from HLT and actually being serviced by the
    /// next step() call require IE to already be set. When serviced, the
    /// CPU pushes P (and, unlike RTN, must resume via RTI so T is
    /// restored) then jumps to the vector at 0xFFFA, the CPU's internal
    /// timer vector.
    void requestMaskableInterrupt();

    /// Unconditional HLT wake, independent of IE -- for the ON key (see
    /// requestMaskableInterrupt()'s own comment on the LH5801's separate,
    /// genuinely-non-maskable line this models). Does not push state or
    /// vector anywhere; see the .cpp comment for why.
    void wakeFromHalt();

    // ── Trace / debug API ─────────────────────────────────────────────────
    // Zero overhead when disabled: step() hot path costs one atomic load;
    // falls through with no extra work when traceFlags() == TRACE_NONE.
    void     setTraceFlags(uint32_t flags) { m_traceFlags.store(flags, std::memory_order_relaxed); }
    uint32_t traceFlags() const { return m_traceFlags.load(std::memory_order_relaxed); }

    /// Drain up to `max` frames from the ring (oldest first since the last
    /// drain). If frames were overwritten before being drained, *outLost is
    /// set to the count lost. Returns the number of frames written to `out`.
    uint32_t drainTraceEvents(CpuFrame* out, uint32_t max, uint32_t* outLost);

    /// Read up to the `max` most-recently-written frames (oldest of that
    /// set first) WITHOUT consuming them -- unlike drainTraceEvents(),
    /// repeated calls return overlapping/identical data as long as no new
    /// frames have been written. For a debugger's frozen view: freeze,
    /// call once to capture a stable snapshot, and the live drain cursor
    /// (and thus the next unfrozen drainTraceEvents() call) is untouched.
    uint32_t peekTraceEvents(CpuFrame* out, uint32_t max);

    void addBreakpoint(uint16_t addr);
    void removeBreakpoint(uint16_t addr);
    void clearBreakpoints();
    /// Returns true once per hit; call after each step() that returned 0.
    bool consumeBreakpointHit();

    /// True if the most recently executed opcode had no case in execute()/
    /// executeFD() (i.e. isn't in the LH5801_Guide.md instruction set this
    /// core was built from). Execution still proceeds as a no-op rather than
    /// halting — real hardware behavior for undocumented opcodes isn't
    /// sourced — but this makes that distinguishable from a genuinely
    /// emulated NOP instead of silently vanishing into the trace.
    bool consumeIllegalOpcodeHit();
    /// The (pc, opcode) of the illegal opcode consumeIllegalOpcodeHit() just
    /// reported. opcode is 0xFD00|byte2 for an FD-prefixed illegal opcode.
    uint16_t lastIllegalOpcodePC() const { return m_lastIllegalOpcodePC; }
    uint16_t lastIllegalOpcode() const { return m_lastIllegalOpcode; }

    /// Tags every CpuFrame this instance records with the given CPU-id --
    /// CPU_ID_UNSPECIFIED by default, which is correct for the PC-1500/
    /// 1500A's standalone LH5801.
    /// PC1600Machine calls this with CPU_ID_LH5803 on its LH5803 instance
    /// so the two CPUs' trace rings can be told apart once merged into one
    /// stream at the GUI/tooling layer.
    void setCpuIdTag(uint8_t tag) { m_cpuIdTag = tag; }

private:
    LH5801Bus& bus;

    // ── Registers ──────────────────────────────────────────────────────
    uint8_t  A{0};
    uint8_t  XL{0}, XH{0}, YL{0}, YH{0}, UL{0}, UH{0};
    uint16_t S{0};
    uint16_t P{0};
    uint8_t  T{0};             // bit0=C,1=IE,2=Z,3=V,4=H; bits 7:5 always 0
    bool     PU{false}, PV{false};
    bool     DISP{false};
    uint16_t TM{0};            // 9-bit timer counter (bit 8 in bit position 8)
    bool     m_halted{false};
    bool     m_poweredOff{false};
    bool     m_irqPending{false};

    void setFlagBit(uint8_t mask, bool v) { if (v) T |= mask; else T &= uint8_t(~mask); }

    // ── Register-pair helpers (0=X,1=Y,2=U) ─────────────────────────────
    uint8_t  lowOf(int pair) const;
    uint8_t  highOf(int pair) const;
    void     setLowOf(int pair, uint8_t v);
    void     setHighOf(int pair, uint8_t v);

    // ── Fetch helpers ────────────────────────────────────────────────────
    uint8_t  fetch8();
    uint16_t fetch16(); // big-endian: high byte first, then low

    // ── ALU / flag helpers ───────────────────────────────────────────────
    void setZFlagFrom(uint8_t result) { setFlagBit(0x04, result == 0); }
    /// General 8-bit add: result = a + operand + carryIn. carryIn is used
    /// literally (0 or 1) -- callers that want "include the existing C
    /// flag" (ADC) must pass T&1 explicitly; callers that want a clean
    /// add with no incoming carry (ADI/INC) pass 0. Sets C/V/H/Z; returns
    /// the result (caller decides where it goes). Taking carryIn as an
    /// explicit literal (rather than an internal re-read of T&1) lets
    /// callers like aluSub's DEC path force a specific carry-in (1, for
    /// "no borrow") regardless of T's current state.
    uint8_t aluAdd(uint8_t a, uint8_t operand, uint8_t carryIn);
    /// General 8-bit subtract via complement-add: result = a + ~operand + carryIn.
    /// C=1 means "no borrow" (LH5801 convention). Sets C/V/H/Z. Same
    /// literal-carryIn convention as aluAdd -- see its comment.
    uint8_t aluSub(uint8_t a, uint8_t operand, uint8_t carryIn);
    uint8_t bcdAdd(uint8_t a, uint8_t operand, bool carryIn);
    uint8_t bcdSub(uint8_t a, uint8_t operand, uint8_t carryIn);

    // ── Instruction execution ────────────────────────────────────────────
    int execute(uint8_t opcode);
    int executeFD(uint8_t opcode); // opcode is the byte after the 0xFD prefix
    void doBranch(bool forward, uint8_t e);
    static uint8_t drlMerge(uint8_t memOld, uint8_t aOld) { return uint8_t(((memOld & 0x0F) << 4) | ((aOld & 0xF0) >> 4)); }
    static uint8_t drrMerge(uint8_t memOld, uint8_t aOld) { return uint8_t(((aOld & 0x0F) << 4) | ((memOld & 0xF0) >> 4)); }
    void pushByte(uint8_t v);
    uint8_t popByte();
    void push16(uint16_t v); // high byte first
    uint16_t pop16();
    void vectorCall(uint8_t index); // pushes P, jumps to ME0[0xFF00+index] (16-bit, big-endian)
    void serviceInterrupt();
    void tickTimer(int cycles); // advances TM through its LFSR sequence by elapsed cycles/kCyclesPerTick; requests a timer interrupt on reaching 0x1FF
    uint32_t m_timerCycleAccumulator{0}; // cycles accumulated toward the next LFSR step

    // ── Trace / debug state ──────────────────────────────────────────────
    std::atomic<uint32_t> m_traceFlags{TRACE_NONE};
    uint32_t m_traceSeqno{0};
    static constexpr uint32_t kRingSize = 512;
    static constexpr uint32_t kRingMask = kRingSize - 1;
    CpuFrame m_ring[kRingSize]{};
    uint32_t m_drainCursor{0};   // next read index
    uint32_t m_totalWritten{0};  // write count so far; also gives the next write index (m_totalWritten & kRingMask) and overflow accounting
    mutable std::mutex m_traceMutex;

    std::vector<uint16_t> m_breakpoints; // sorted ascending
    bool m_breakpointHit{false};

    bool     m_illegalOpcodeHit{false};
    uint16_t m_lastIllegalOpcodePC{0};
    uint16_t m_lastIllegalOpcode{0};
    uint8_t  m_cpuIdTag{CPU_ID_UNSPECIFIED};

    void recordTraceFrame(uint32_t tf, uint16_t pcAtStart, uint16_t opcodeWord, uint8_t cycles);
};
