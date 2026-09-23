#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "../../TraceTypes.hpp"

// ── Bus interface ────────────────────────────────────────────────────────
//
// The SC7852 addresses a single 64KB memory space (bank-switched by
// PC1600Bank/PC1600Memory underneath, transparent to this core) plus a
// separate 8-bit I/O port space, standard Z-80
// conventions. PC1600Memory implements the memory side; the on-chip
// 30H-3FH control-register block (Port 31H/28H/3DH bank selects, Port
// 32H/35H/39H interrupt registers, Port 38H CPU-switch trigger) is decoded
// by SC7852 itself (see writeIO()/readIO()) since those registers live on
// the CPU, not the memory decoder.
class SC7852Bus {
public:
    virtual ~SC7852Bus() = default;

    virtual uint8_t readMem(uint16_t addr) = 0;
    virtual void    writeMem(uint16_t addr, uint8_t value) = 0;

    /// `port` is the low 8 bits of the I/O address (the only part any
    /// PC-1600 port decode documented so far depends on -- see
    /// PC-1600-IO-Ports.md). The high 8 bits (A register for `OUT (n),A`/
    /// `IN A,(n)`, or the B register for `OUT (C),r`/`IN r,(C)`) aren't
    /// forwarded since nothing in this project's scope needs them yet.
    virtual uint8_t readIO(uint8_t port) { (void)port; return 0xFF; }
    virtual void    writeIO(uint8_t port, uint8_t value) { (void)port; (void)value; }
};

// ── SC7852 CPU core ──────────────────────────────────────────────────────
//
// Zilog Z-80A-compatible CPU as used in the PC-1600. Standard Z-80A ISA
// and timing, with no PC-1600-specific opcode deltas. The on-chip
// LH5810-compatible I/O port block (10H-1FH) and the 30H-3FH
// bank/interrupt-control block are address-mapped I/O behavior, not ISA
// deltas -- both are just ordinary IN/OUT targets from this core's point of
// view, decoded by whatever's on the far side of readIO()/writeIO().
//
// **One wait state per M1 (opcode fetch) cycle.** The Technical Reference
// Manual says "1 WAIT automatically inserted in the machine cycle" without
// naming which one. Hardware measured 2026-09-23 settles it as M1. The ROM
// BEEP tone loop (P1-B3 5EB9) costs 52*A+389 T-states per period at nominal
// Zilog timing and contains 8*A+52 M1 cycles. A real unit plays A=200 at
// 287.13 Hz and A=50 at 1038.15 Hz. Nominal timing gives 331.8 / 1197.7 Hz.
// One wait per M1 gives 287.8 / 1040.4 Hz. That leaves the same -0.22%
// residual at both pitches, i.e. crystal/recorder tolerance, not a timing
// model error. It also matches TRM §3.10's 1.3M/(166+22A) formula. A wait on
// every M-cycle would be ~3% slower still.
// So every step() adds kM1WaitStates per opcode byte fetched as an M1:
// one for a plain opcode and two for CB/ED/DD/FD-prefixed ones. DD/FD-CB
// forms also count two, because their displacement and sub-opcode are
// ordinary memory reads, not M1 fetches. Interrupt acknowledge adds one,
// and so does each internal NOP while halted.
class SC7852 {
public:
    explicit SC7852(SC7852Bus& bus);

    /// Cold reset: PC=0000H, I=R=0, IFF1=IFF2=0, IM=0, SP=FFFFH -- standard
    /// Z-80 reset state. ELH#/bus-ownership is not this class's concern;
    /// PC1600Machine drives that.
    void reset();

    /// Execute one instruction at the current PC. Returns the T-state
    /// (cycle) count consumed, or 0 if the CPU is halted (HALT) with no
    /// pending interrupt to wake it -- mirrors LH5801::step()'s convention
    /// so PC1600Machine/PC1600BusArbiter can treat both CPUs uniformly.
    int step();

    /// Wait states the SC-7852 inserts into every M1 cycle (see the class
    /// comment).
    static constexpr int kM1WaitStates = 1;

    /// T-states to credit for a step() that returned 0 because the CPU is
    /// halted. A real HALT is not free time: the Z-80 executes an internal
    /// NOP every machine cycle while parked, burning 4 T-states apiece plus
    /// the M1 wait, so
    /// anything deriving emulated wall-clock time from step() costs must
    /// charge idle time at this rate rather than treating "halted" as "no
    /// time passed". Public for exactly that reason -- PC1600Machine's
    /// timer accumulators and its runCycles() budget both use it, and they
    /// have to agree: one paces wall-clock time, the other derives the
    /// 64 Hz timer from it. (Mirrors LH5801::kHaltTickCycles, whose value
    /// differs because that core's step() ticks its timer once per call.)
    static constexpr int kHaltTickCycles = 4 + kM1WaitStates;

    // ── Register access (debug / tests) ──────────────────────────────────
    uint8_t  a() const { return A; }
    uint8_t  f() const { return F; }
    uint16_t af() const { return uint16_t(A << 8) | F; }
    uint16_t bc() const { return uint16_t(B << 8) | C; }
    uint16_t de() const { return uint16_t(D << 8) | E; }
    uint16_t hl() const { return uint16_t(H << 8) | L; }
    uint16_t ix() const { return IX; }
    uint16_t iy() const { return IY; }
    uint16_t sp() const { return SP; }
    uint16_t pc() const { return PC; }
    uint8_t  i()  const { return I; }
    uint8_t  r()  const { return R; }
    bool     iff1() const { return IFF1; }
    bool     iff2() const { return IFF2; }
    uint8_t  im() const { return IM; }
    bool     halted() const { return m_halted; }
    /// Clears HALT without going through the interrupt path -- used by
    /// PC1600BusArbiter/PC1600Machine to resume the SC7852 when bus
    /// ownership switches back to it after parking on
    /// `HALT` as the second half of the documented SC7852→LH5803 handoff
    /// sequence (`OUT (38H),A` then `HALT`). Not a real interrupt --
    /// nothing about this wake is visible to the program the way an
    /// actual interrupt service would be (no PC push, no IFF1 change).
    void     resumeFromHalt() { m_halted = false; }

    void setAF(uint16_t v) { A = uint8_t(v >> 8); F = uint8_t(v); }
    void setBC(uint16_t v) { B = uint8_t(v >> 8); C = uint8_t(v); }
    void setDE(uint16_t v) { D = uint8_t(v >> 8); E = uint8_t(v); }
    void setHL(uint16_t v) { H = uint8_t(v >> 8); L = uint8_t(v); }
    void setIX(uint16_t v) { IX = v; }
    void setIY(uint16_t v) { IY = v; }
    void setSP(uint16_t v) { SP = v; }
    void setPC(uint16_t v) { PC = v; }

    bool flagS()  const { return (F & 0x80) != 0; }
    bool flagZ()  const { return (F & 0x40) != 0; }
    bool flagH()  const { return (F & 0x10) != 0; }
    bool flagPV() const { return (F & 0x04) != 0; }
    bool flagN()  const { return (F & 0x02) != 0; }
    bool flagC()  const { return (F & 0x01) != 0; }

    /// Maskable interrupt request (INT). Serviced at the start of the next
    /// step() call if IFF1 is set and the previous instruction was not EI;
    /// otherwise it stays pending (a HALTed CPU stays HALTed) until
    /// interrupts are enabled -- real Z-80 behavior.
    /// PC-1600's IM2 vector byte (Port 39H, low byte of the vector address;
    /// I register supplies the high byte) is the caller's responsibility to
    /// have wired up via setIM2VectorByte() before requesting.
    void requestInterrupt();
    void setIM2VectorByte(uint8_t low) { m_im2VectorLow = low; }

    /// Non-maskable interrupt (NMI): always serviced, clears IFF1 (saving
    /// its prior value into IFF2 per standard Z-80 semantics), vectors to
    /// 0066H. Not known to be used anywhere in the PC-1600's documented
    /// interrupt model (Port 32H/35H's 7-source cause/mask is entirely
    /// maskable-interrupt-based) -- provided for completeness/parity with
    /// the real chip, not because anything in this project's scope raises it.
    void requestNMI();

    // ── Trace / debug API (mirrors LH5801's) ─────────
    // Zero overhead when disabled: step() hot path costs one atomic load;
    // falls through with no extra work when traceFlags() == TRACE_NONE.
    void     setTraceFlags(uint32_t flags) { m_traceFlags.store(flags, std::memory_order_relaxed); }
    uint32_t traceFlags() const { return m_traceFlags.load(std::memory_order_relaxed); }

    uint32_t drainTraceEvents(Z80CpuFrame* out, uint32_t max, uint32_t* outLost);
    uint32_t peekTraceEvents(Z80CpuFrame* out, uint32_t max);

    void addBreakpoint(uint16_t addr);
    void removeBreakpoint(uint16_t addr);
    void clearBreakpoints();
    /// Returns true once per hit; call after each step() that returned 0.
    bool consumeBreakpointHit();

private:
    SC7852Bus& bus;

    // ── Registers ─────────────────────────────────────────────────────
    uint8_t A{0xFF}, F{0xFF}, B{0}, C{0}, D{0}, E{0}, H{0}, L{0};
    uint8_t A2{0xFF}, F2{0xFF}, B2{0}, C2{0}, D2{0}, E2{0}, H2{0}, L2{0};
    uint16_t IX{0}, IY{0}, SP{0xFFFF}, PC{0};
    uint8_t I{0}, R{0};
    bool IFF1{false}, IFF2{false};
    uint8_t IM{0};
    bool m_halted{false};
    bool m_irqPending{false};
    bool m_nmiPending{false};
    bool m_eiShadow{false};   // set by EI: blocks INT acceptance for one instruction
    uint8_t m_im2VectorLow{0xFF};

    // ── Fetch helpers ────────────────────────────────────────────────────
    uint8_t  fetch8();
    uint16_t fetch16(); // little-endian: low byte first, then high
    void     bumpR() { R = uint8_t((R & 0x80) | ((R + 1) & 0x7F)); }

    // ── Flag helpers ─────────────────────────────────────────────────────
    static bool parity(uint8_t v);
    void setFlagBit(uint8_t mask, bool v) { if (v) F |= mask; else F &= uint8_t(~mask); }
    void setSZ53(uint8_t result); // S,Z + undocumented bits 5/3 from result
    void setSZP53(uint8_t result); // as above, plus PV=parity(result)

    uint8_t  aluAdd8(uint8_t a, uint8_t operand, uint8_t carryIn, bool isSub = false);
    uint8_t  add8(uint8_t a, uint8_t operand) { return aluAdd8(a, operand, 0, false); }
    uint8_t  adc8(uint8_t a, uint8_t operand) { return aluAdd8(a, operand, F & 0x01, false); }
    uint8_t  sub8(uint8_t a, uint8_t operand) { return aluAdd8(a, operand, 0, true); }
    uint8_t  sbc8(uint8_t a, uint8_t operand) { return aluAdd8(a, operand, F & 0x01, true); }
    void     cp8(uint8_t a, uint8_t operand); // like sub8 but discards the result
    uint8_t  inc8(uint8_t v);
    uint8_t  dec8(uint8_t v);
    void     and8(uint8_t operand);
    void     or8(uint8_t operand);
    void     xor8(uint8_t operand);
    uint16_t addHL16(uint16_t hlv, uint16_t operand); // ADD HL,rr: H,N,C only, no S/Z/PV
    uint16_t adc16(uint16_t hlv, uint16_t operand);
    uint16_t sbc16(uint16_t hlv, uint16_t operand);
    void     daa();

    uint8_t  rlc(uint8_t v); uint8_t rrc(uint8_t v);
    uint8_t  rl(uint8_t v);  uint8_t rr(uint8_t v);
    uint8_t  sla(uint8_t v); uint8_t sra(uint8_t v);
    uint8_t  sll(uint8_t v); uint8_t srl(uint8_t v);

    // ── Stack ────────────────────────────────────────────────────────────
    void pushWord(uint16_t v);
    uint16_t popWord();

    // ── Register-pair table helpers (for decoding r/rr fields) ──────────
    uint8_t  readReg8(int code);         // 0=B 1=C 2=D 3=E 4=H 5=L 6=(HL) 7=A
    void     writeReg8(int code, uint8_t v);
    uint16_t readReg16_sp(int code) const;  // 0=BC 1=DE 2=HL 3=SP (for LD/INC/DEC/ADD-rr forms)
    void     writeReg16_sp(int code, uint16_t v);
    uint16_t readReg16_af(int code) const;  // 0=BC 1=DE 2=HL 3=AF (for PUSH/POP)
    void     writeReg16_af(int code, uint16_t v);

    bool condTrue(int code) const; // 0=NZ 1=Z 2=NC 3=C 4=PO 5=PE 6=P 7=M

    // ── Instruction execution ───────────────────────────────────────────
    int execute(uint8_t opcode);        // unprefixed
    int executeCB(uint8_t opcode);      // 0xCB prefix
    int executeED(uint8_t opcode);      // 0xED prefix
    int executeDDFD(uint8_t opcode, uint16_t& ixy); // 0xDD/0xFD prefix; ixy is IX or IY by reference
    int executeDDFDCB(uint8_t opcode, uint16_t ixy, int8_t d); // 0xDD/0xFD 0xCB d opcode

    /// Returns the cycle cost if an interrupt was actually serviced (PC
    /// redirected to a handler) this call, or -1 if nothing happened (no
    /// interrupt pending, or only a maskable one that IFF1 or
    /// `maskableBlocked` -- the EI shadow -- holds off). step() uses -1 to mean "go ahead
    /// and fetch/execute a normal opcode this call" -- servicing an
    /// interrupt and executing the next opcode never happen in the same
    /// step() call, matching real hardware (the interrupt ack cycle IS
    /// the whole "instruction" for that cycle).
    int serviceInterrupt(bool maskableBlocked);

    // ── Trace / debug state (mirrors LH5801's exactly, Z80CpuFrame-shaped) ──
    std::atomic<uint32_t> m_traceFlags{TRACE_NONE};
    uint32_t m_traceSeqno{0};
    // Needs to be large relative to the LH5801's ring size: at PC1600's
    // ~1.3 MHz emulated clock and a Z80's short (~4-20 T-state)
    // instructions, one 60 Hz GUI tick (EmulatorViewModel.tick(), which
    // drains this ring) can correspond to several thousand instructions,
    // and the background emulation loop (EmulatorViewModel's emulQueue, a
    // separate ~20ms-batch timer) keeps producing frames regardless of
    // whether the main thread's tick() is keeping up -- any stall on the
    // main thread (rendering, other work) lets the ring wrap many times
    // before the next drain. 65536 gives ample headroom (~1.9 MB, trivial)
    // -- see also EmulatorViewModel.drainTraceEventsPC1600()'s matching
    // drain-buffer size, which must stay >= this to actually empty the
    // ring each tick.
    static constexpr uint32_t kRingSize = 65536;
    static constexpr uint32_t kRingMask = kRingSize - 1;
    Z80CpuFrame m_ring[kRingSize]{};
    uint32_t m_drainCursor{0};
    uint32_t m_totalWritten{0};
    mutable std::mutex m_traceMutex;

    std::vector<uint16_t> m_breakpoints; // sorted ascending
    bool m_breakpointHit{false};

    void recordTraceFrame(uint32_t tf, uint16_t pcAtStart, uint16_t opcodeWord, uint8_t cycles);
};
