#include "LH5801.hpp"

#include <algorithm>

#include "LH5801TimerTable.hpp"

namespace {
// Sentinel returned by execute()/executeFD() for an opcode with no case —
// never a real cycle count (those are all small positive values per the
// LH5801_Guide.md tables). step() catches this, records it as an illegal-
// opcode event, and substitutes a plain pacing value so execution continues.
constexpr int kIllegalOpcode = -1;

// Interrupt acknowledge: push P (2) + push T (1) + vector fetch (2), no
// cycle count documented for this path -- one named constant so the
// "consumed this many cycles" and "return this many cycles" sites in
// step() can't drift apart from each other.
constexpr int kInterruptAckCycles = 14;
} // namespace

// Opcode/cycle/flag data below is transcribed directly from
// SharpPC1500Reference/Assembly-Programming/LH5801_Guide.md's "Instruction
// Set Reference" section. Two points where the guide's prose was
// internally ambiguous are flagged at their point of use below: DRL/DRR
// nibble rotation, and CPA/CPI/CIN's carry-in (see the CPA case below).

LH5801::LH5801(LH5801Bus& busRef) : bus(busRef) {
    reset();
}

// ── Register-pair helpers ────────────────────────────────────────────────

uint8_t LH5801::lowOf(int pair) const {
    switch (pair) {
        case 0: return XL;
        case 1: return YL;
        default: return UL;
    }
}
uint8_t LH5801::highOf(int pair) const {
    switch (pair) {
        case 0: return XH;
        case 1: return YH;
        default: return UH;
    }
}
void LH5801::setLowOf(int pair, uint8_t v) {
    switch (pair) {
        case 0: XL = v; break;
        case 1: YL = v; break;
        default: UL = v; break;
    }
}
void LH5801::setHighOf(int pair, uint8_t v) {
    switch (pair) {
        case 0: XH = v; break;
        case 1: YH = v; break;
        default: UH = v; break;
    }
}
// ── Reset ─────────────────────────────────────────────────────────────────

void LH5801::reset() {
    A = XL = XH = YL = YH = UL = UH = 0;
    S = 0;
    T = 0;
    PU = PV = false;
    DISP = false;
    TM = 0;
    m_timerCycleAccumulator = 0;
    m_halted = false;
    m_poweredOff = false;
    m_irqPending = false;
    m_history.clear();
    // 16-bit big-endian reset vector at ME0 0xFFFE/0xFFFF (confirmed against
    // PC-1500_A04.ROM: bytes E0,00 -> 0xE000, which decodes as RIE; LDI A,0;
    // AM0; RDP; ... — a plausible reset-init sequence).
    uint8_t hi = bus.readME0(0xFFFE);
    uint8_t lo = bus.readME0(0xFFFF);
    P = (uint16_t(hi) << 8) | lo;
}

// ── Fetch ─────────────────────────────────────────────────────────────────

uint8_t LH5801::fetch8() {
    uint8_t v = bus.readME0(P);
    P = uint16_t(P + 1);
    if (m_fetchLen < sizeof(LH5801HistoryFrame::bytes)) m_history.next().bytes[m_fetchLen] = v;
    m_fetchLen++;
    return v;
}
uint16_t LH5801::fetch16() {
    uint8_t hi = fetch8();
    uint8_t lo = fetch8();
    return (uint16_t(hi) << 8) | lo;
}

// ── ALU helpers ───────────────────────────────────────────────────────────

uint8_t LH5801::aluAdd(uint8_t a, uint8_t operand, uint8_t carryIn) {
    uint8_t cin = carryIn & 1;
    uint16_t sum16 = uint16_t(a) + operand + cin;
    uint8_t halfSum = uint8_t((a & 0x0F) + (operand & 0x0F) + cin);
    uint8_t result = uint8_t(sum16);
    bool c = sum16 > 0xFF;
    bool h = halfSum > 0x0F;
    bool v = (((a ^ operand) & 0x80) == 0) && (((a ^ result) & 0x80) != 0);
    setFlagBit(0x01, c);
    setFlagBit(0x10, h);
    setFlagBit(0x08, v);
    setZFlagFrom(result);
    return result;
}

uint8_t LH5801::aluSub(uint8_t a, uint8_t operand, uint8_t carryIn) {
    // a - operand - NOT(C) via complement-add; C-out here is "no borrow".
    return aluAdd(a, uint8_t(~operand), carryIn);
}

// Packed-BCD add/sub. The guide describes DCA's hardware algorithm only in
// terms of internal micro-steps ("a=a+0x66; a=a+op+C; a=a+DA") without
// spelling out the DA compensation table, so this uses the standard
// decimal-adjust-after-add/sub technique (equivalent in observable result:
// correct packed-BCD sum/difference with C/H reflecting decade carry/borrow).
uint8_t LH5801::bcdAdd(uint8_t a, uint8_t operand, bool carryIn) {
    uint8_t cin = carryIn ? 1 : 0;
    uint8_t lowSum = uint8_t((a & 0x0F) + (operand & 0x0F) + cin);
    bool h = lowSum > 9;
    uint16_t sum16 = uint16_t(a) + operand + cin;
    if (h) sum16 += 0x06;
    bool highCarry = ((sum16 >> 4) & 0x0F) > 9 || sum16 > 0xFF;
    if (highCarry) sum16 += 0x60;
    bool c = sum16 > 0xFF;
    uint8_t result = uint8_t(sum16);
    bool v = (((a ^ operand) & 0x80) == 0) && (((a ^ result) & 0x80) != 0);
    setFlagBit(0x01, c);
    setFlagBit(0x10, h);
    setFlagBit(0x08, v);
    setZFlagFrom(result);
    return result;
}
uint8_t LH5801::bcdSub(uint8_t a, uint8_t operand, uint8_t carryIn) {
    uint8_t cin = carryIn;
    int lowDiff = (a & 0x0F) - (operand & 0x0F) - (1 - cin);
    bool h = lowDiff < 0;
    int diff = int(a) - int(operand) - (1 - cin);
    if (h) diff -= 0x06;
    bool borrow = diff < 0;
    if (borrow) diff -= 0x60;
    uint8_t result = uint8_t(diff & 0xFF);
    bool c = !borrow; // no-borrow convention
    bool v = (((a ^ operand) & 0x80) != 0) && (((a ^ result) & 0x80) != 0);
    setFlagBit(0x01, c);
    setFlagBit(0x10, h);
    setFlagBit(0x08, v);
    setZFlagFrom(result);
    return result;
}

// ── Stack ─────────────────────────────────────────────────────────────────

void LH5801::pushByte(uint8_t v) { bus.writeME0(S, v); S = uint16_t(S - 1); }
uint8_t LH5801::popByte() { S = uint16_t(S + 1); return bus.readME0(S); }
void LH5801::push16(uint16_t v) { pushByte(uint8_t(v)); pushByte(uint8_t(v >> 8)); } // low first onto stack top-down: PL then PH per guide's SJP description
uint16_t LH5801::pop16() { uint8_t hi = popByte(); uint8_t lo = popByte(); return (uint16_t(hi) << 8) | lo; }

void LH5801::vectorCall(uint8_t index) {
    push16(P);
    uint16_t base = uint16_t(0xFF00 + index);
    uint8_t hi = bus.readME0(base);
    uint8_t lo = bus.readME0(uint16_t(base + 1));
    P = (uint16_t(hi) << 8) | lo;
    setFlagBit(0x04, false); // Z forced reset per guide
}

void LH5801::doBranch(bool forward, uint8_t e) {
    P = forward ? uint16_t(P + e) : uint16_t(P - e);
}

// ── Interrupts ────────────────────────────────────────────────────────────

void LH5801::requestMaskableInterrupt() {
    m_irqPending = true;
    // Both waking from HLT and actually servicing (see step()) require IE
    // for this class of interrupt (timer/general maskable). The LH5801
    // also has a separate, genuinely-non-maskable NMI line (vector 0xFFFC)
    // that this core does not model, since nothing exercised so far
    // requires it.
    if (flagIE()) m_halted = false;
}

void LH5801::wakeFromHalt() {
    // The unconditional counterpart to requestMaskableInterrupt() above --
    // for the LH5801's genuinely-non-maskable class of wake (the ON key's
    // real wiring: straight to a power-on latch/BFI pin, not the ordinary
    // IE-gated maskable-IRQ line). No vector jump here: this core has no
    // confirmed ISR address for the LH5801's own NMI line (0xFFFC, not
    // modeled -- see requestMaskableInterrupt()'s own comment), and
    // jumping to an unconfirmed address risks landing worse than simply
    // resuming normal fetch right after the HLT, which is what an
    // interrupt-driven HLT wake looks like when nothing ends up servicing
    // it.
    m_halted = false;
}

void LH5801::serviceInterrupt() {
    m_irqPending = false;
    // RTI pops P (high,low) then T last, so T must be pushed first here —
    // push16(P) pushes low-then-high, leaving PH as the most recent (first-
    // popped) byte, with T deepest (popped last), mirroring RTI exactly.
    pushByte(T);
    push16(P);
    // Acceptance resets IE (RTI restores it from the pushed T). The ROM's
    // MI handler at E171 pushes A/X/Y/U before clearing the level-held
    // request at F00B, which only works if IE is already off on entry.
    setFlagBit(0x02, false);
    uint8_t hi = bus.readME0(0xFFFA);
    uint8_t lo = bus.readME0(0xFFFB);
    P = (uint16_t(hi) << 8) | lo;
}

// ── Trace ─────────────────────────────────────────────────────────────────

void LH5801::pushTraceFrame(uint32_t tf, uint16_t pcAtStart, uint16_t opcodeWord, uint8_t cycles) {
    CpuFrame f{};
    f.seqno = m_traceSeqno++;
    f.pc = pcAtStart;
    f.opcode = opcodeWord;
    f.cycles = cycles;
    f.cpuId = m_cpuIdTag;
    if (tf & (TRACE_REGS_LIGHT | TRACE_REGS_FULL)) {
        f.a = A; f.xl = XL; f.xh = XH; f.yl = YL; f.yh = YH; f.ul = UL; f.uh = UH;
        f.s = S; f.t = T;
    }
    if (tf & TRACE_REGS_FULL) {
        f.pu = PU; f.pv = PV; f.disp = DISP; f.tm = TM;
    }

    m_trace.push(f);
}

void LH5801::recordHistory(uint16_t pcAtStart, uint8_t cycles, bool interrupt) {
    LH5801HistoryFrame& h = m_history.next(); // bytes[] already filled by fetch8()
    h.pc = pcAtStart;
    h.len = interrupt ? 0 : uint8_t(m_fetchLen < sizeof(h.bytes) ? m_fetchLen : sizeof(h.bytes));
    h.cycles = cycles;
    h.interrupt = interrupt;
    h.a = A; h.x = x(); h.y = y(); h.u = u(); h.s = S; h.p = P; h.t = T;
    h.pu = PU; h.pv = PV;
    m_history.commit();
}

bool LH5801::consumeIllegalOpcodeHit() {
    bool hit = m_illegalOpcodeHit;
    m_illegalOpcodeHit = false;
    return hit;
}

// ── step() ────────────────────────────────────────────────────────────────

int LH5801::step() {
    // Genuine power-down (OFF instruction, 0xFD 0x4C) takes priority over
    // everything else in step(), including a pending interrupt: real
    // hardware has physically cut the CPU clock, so nothing -- not even
    // the internal timer that would otherwise end a HLT -- can run or tick
    // until powerOn() (the ON key's BFI pin) restores it. See executeFD()'s
    // case 0x4C and poweredOff()'s own doc comment.
    if (m_poweredOff) return 0;
    // Servicing a maskable interrupt (pushing state and vectoring) is gated
    // on IE, and so is the HLT wake in requestMaskableInterrupt() -- a
    // request made while IE=0 sits pending (a HLT stays halted) until IE
    // is later set (e.g. by SIE or RTI) and a subsequent step() call
    // finally consumes it.
    if (m_irqPending && flagIE()) {
        const uint16_t interruptedP = P;
        serviceInterrupt();
        recordHistory(interruptedP, uint8_t(kInterruptAckCycles), true);
        // Interrupt acknowledge consumes this step() call on its own —
        // the handler's first instruction executes on the *next* step(),
        // starting cleanly at the vector address. Keeps one step() ==
        // one instruction (or one interrupt-entry event) for the trace
        // ring and single-step debugging.
        tickTimer(kInterruptAckCycles);
        return kInterruptAckCycles;
    }
    if (m_halted) {
        // The timer's crystal/divider keeps running even while the CPU is
        // halted (real hardware: only instruction fetch/execute pauses) --
        // that's the entire point of a HLT-then-timer-interrupt idle loop
        // like the ROM's own "IDLE" address (confirmed real behavior: see
        // Core/tests/lh5801_tests.cpp's boot smoke test). Advance the timer
        // by one tick unit per call so a loaded TM eventually wakes us;
        // still returns 0 (no instruction executed) per this method's
        // documented contract -- kHaltTickCycles is exposed so
        // PC1500Machine::runCycles() can budget its own idle polling
        // without depending on step()'s return value changing meaning.
        tickTimer(kHaltTickCycles);
        return 0; // remains halted until an interrupt wakes it (see requestMaskableInterrupt)
    }

    uint32_t tf = traceFlags();
    if (tf & TRACE_BREAKPOINTS) {
        if (m_breakpoints.check(P)) return 0;
    }

    uint16_t pcAtStart = P;
    m_fetchLen = 0;
    uint8_t opcode = fetch8();
    uint16_t opcodeWord = opcode;
    int cycles;
    if (opcode == 0xFD) {
        uint8_t opcode2 = fetch8();
        opcodeWord = uint16_t(0xFD00 | opcode2);
        cycles = executeFD(opcode2);
    } else {
        cycles = execute(opcode);
    }

    if (cycles == kIllegalOpcode) {
        m_illegalOpcodeHit = true;
        m_lastIllegalOpcodePC = pcAtStart;
        m_lastIllegalOpcode = opcodeWord;
        cycles = 5; // plain pacing value so execution still advances
    }

    recordTraceFrame(tf, pcAtStart, opcodeWord, uint8_t(cycles));
    recordHistory(pcAtStart, uint8_t(cycles), false);
    tickTimer(cycles);
    return cycles;
}

// TM is a 9-bit LFSR (polynomial) counter, not a linear one -- see
// LH5801TimerTable.hpp for the full derivation and provenance. Confirmed
// to request a maskable interrupt at vector 0xFFFA each time it reaches
// 0x1FF, then keeps free-running through the same 511-state cycle
// indefinitely (firing again every ~511 ticks) until software reloads TM
// (including to 0, which parks/stops it) via AM0/AM1. TM==0 means
// "stopped," matching LH5801_Guide.md's own wording for that one case.
void LH5801::tickTimer(int cycles) {
    if (TM == 0 || cycles <= 0) return;
    m_timerCycleAccumulator += uint32_t(cycles);
    while (m_timerCycleAccumulator >= uint32_t(lh5801timer::kCyclesPerTick)) {
        m_timerCycleAccumulator -= uint32_t(lh5801timer::kCyclesPerTick);
        // TM's own value is the sole source of truth for LFSR position --
        // kPolyCounterReverse[TM] gives it back in O(1), so there's no
        // need to separately track and keep an index in sync by hand.
        uint16_t step = lh5801timer::kPolyCounterReverse[TM];
        TM = lh5801timer::kPolyCounterTable[(step + 1) % 511];
        if (TM == 0x1FF) requestMaskableInterrupt(); // masking is applied at service time, not here -- see step()
    }
}

void LH5801::setTimer(uint16_t v) { TM = v; }

// ── Primary (non-FD) opcode dispatch ────────────────────────────────────────

int LH5801::execute(uint8_t op) {
    switch (op) {
        // ── SBC (reg forms) ──────────────────────────────────────────────
        case 0x00: A = aluSub(A, XL, T & 1); return 6;
        case 0x10: A = aluSub(A, YL, T & 1); return 6;
        case 0x20: A = aluSub(A, UL, T & 1); return 6;
        case 0x80: A = aluSub(A, XH, T & 1); return 6;
        case 0x90: A = aluSub(A, YH, T & 1); return 6;
        case 0xA0: A = aluSub(A, UH, T & 1); return 6;
        // SBC (Rreg) / (pp), ME0
        case 0x01: A = aluSub(A, bus.readME0(x()), T & 1); return 7;
        case 0x11: A = aluSub(A, bus.readME0(y()), T & 1); return 7;
        case 0x21: A = aluSub(A, bus.readME0(u()), T & 1); return 7;
        case 0xA1: { uint16_t pp = fetch16(); A = aluSub(A, bus.readME0(pp), T & 1); return 13; }

        // ── ADC (reg forms) ──────────────────────────────────────────────
        case 0x02: A = aluAdd(A, XL, T & 1); return 6;
        case 0x12: A = aluAdd(A, YL, T & 1); return 6;
        case 0x22: A = aluAdd(A, UL, T & 1); return 6;
        case 0x82: A = aluAdd(A, XH, T & 1); return 6;
        case 0x92: A = aluAdd(A, YH, T & 1); return 6;
        case 0xA2: A = aluAdd(A, UH, T & 1); return 6;
        case 0x03: A = aluAdd(A, bus.readME0(x()), T & 1); return 7;
        case 0x13: A = aluAdd(A, bus.readME0(y()), T & 1); return 7;
        case 0x23: A = aluAdd(A, bus.readME0(u()), T & 1); return 7;
        case 0xA3: { uint16_t pp = fetch16(); A = aluAdd(A, bus.readME0(pp), T & 1); return 13; }

        // ── LDA (reg forms) ──────────────────────────────────────────────
        case 0x04: A = XL; setZFlagFrom(A); return 5;
        case 0x14: A = YL; setZFlagFrom(A); return 5;
        case 0x24: A = UL; setZFlagFrom(A); return 5;
        case 0x84: A = XH; setZFlagFrom(A); return 5;
        case 0x94: A = YH; setZFlagFrom(A); return 5;
        case 0xA4: A = UH; setZFlagFrom(A); return 5;
        case 0x05: A = bus.readME0(x()); setZFlagFrom(A); return 6;
        case 0x15: A = bus.readME0(y()); setZFlagFrom(A); return 6;
        case 0x25: A = bus.readME0(u()); setZFlagFrom(A); return 6;
        case 0xA5: { uint16_t pp = fetch16(); A = bus.readME0(pp); setZFlagFrom(A); return 12; }

        // ── CPA (reg forms) ──────────────────────────────────────────────
        // CPA always uses a *forced* carry-in of 1 (i.e. as if `sec` ran
        // immediately before it), never the CPU's actual current C flag.
        // This matters for the ROM's own RAM-presence-test loop, which
        // compares a just-written test byte against a fresh read-back to
        // detect whether an address is real RAM: a stray non-1 carry-in
        // from whatever ran earlier would flip the comparison's Z result
        // even though the bytes genuinely match.
        case 0x06: aluSub(A, XL, 1); return 6;
        case 0x16: aluSub(A, YL, 1); return 6;
        case 0x26: aluSub(A, UL, 1); return 6;
        case 0x86: aluSub(A, XH, 1); return 6;
        case 0x96: aluSub(A, YH, 1); return 6;
        case 0xA6: aluSub(A, UH, 1); return 6;
        case 0x07: aluSub(A, bus.readME0(x()), 1); return 7;
        case 0x17: aluSub(A, bus.readME0(y()), 1); return 7;
        case 0x27: aluSub(A, bus.readME0(u()), 1); return 7;
        case 0xA7: { uint16_t pp = fetch16(); aluSub(A, bus.readME0(pp), 1); return 13; }

        // ── STA ───────────────────────────────────────────────────────────
        case 0x08: XH = A; return 5;
        case 0x18: YH = A; return 5;
        case 0x28: UH = A; return 5;
        case 0x0A: XL = A; return 5;
        case 0x1A: YL = A; return 5;
        case 0x2A: UL = A; return 5;
        case 0x0E: bus.writeME0(x(), A); return 6;
        case 0x1E: bus.writeME0(y(), A); return 6;
        case 0x2E: bus.writeME0(u(), A); return 6;
        case 0xAE: { uint16_t pp = fetch16(); bus.writeME0(pp, A); return 12; }

        // ── AND ───────────────────────────────────────────────────────────
        case 0x09: A &= bus.readME0(x()); setZFlagFrom(A); return 7;
        case 0x19: A &= bus.readME0(y()); setZFlagFrom(A); return 7;
        case 0x29: A &= bus.readME0(u()); setZFlagFrom(A); return 7;
        case 0xA9: { uint16_t pp = fetch16(); A &= bus.readME0(pp); setZFlagFrom(A); return 13; }

        // ── ORA ───────────────────────────────────────────────────────────
        case 0x0B: A |= bus.readME0(x()); setZFlagFrom(A); return 7;
        case 0x1B: A |= bus.readME0(y()); setZFlagFrom(A); return 7;
        case 0x2B: A |= bus.readME0(u()); setZFlagFrom(A); return 7;
        case 0xAB: { uint16_t pp = fetch16(); A |= bus.readME0(pp); setZFlagFrom(A); return 13; }

        // ── EOR ───────────────────────────────────────────────────────────
        case 0x0D: A ^= bus.readME0(x()); setZFlagFrom(A); return 7;
        case 0x1D: A ^= bus.readME0(y()); setZFlagFrom(A); return 7;
        case 0x2D: A ^= bus.readME0(u()); setZFlagFrom(A); return 7;
        case 0xAD: { uint16_t pp = fetch16(); A ^= bus.readME0(pp); setZFlagFrom(A); return 13; }

        // ── BIT ───────────────────────────────────────────────────────────
        case 0x0F: setZFlagFrom(uint8_t(A & bus.readME0(x()))); return 7;
        case 0x1F: setZFlagFrom(uint8_t(A & bus.readME0(y()))); return 7;
        case 0x2F: setZFlagFrom(uint8_t(A & bus.readME0(u()))); return 7;
        case 0xAF: { uint16_t pp = fetch16(); setZFlagFrom(uint8_t(A & bus.readME0(pp))); return 13; }

        // ── DCA / DCS ─────────────────────────────────────────────────────
        case 0x8C: A = bcdAdd(A, bus.readME0(x()), T & 1); return 15;
        case 0x9C: A = bcdAdd(A, bus.readME0(y()), T & 1); return 15;
        case 0xAC: A = bcdAdd(A, bus.readME0(u()), T & 1); return 15;
        case 0x0C: A = bcdSub(A, bus.readME0(x()), T & 1); return 13;
        case 0x1C: A = bcdSub(A, bus.readME0(y()), T & 1); return 13;
        case 0x2C: A = bcdSub(A, bus.readME0(u()), T & 1); return 13;

        // ── SIN / SDE / LIN / LDE ─────────────────────────────────────────
        case 0x41: bus.writeME0(x(), A); setX(uint16_t(x() + 1)); return 6;
        case 0x51: bus.writeME0(y(), A); setY(uint16_t(y() + 1)); return 6;
        case 0x61: bus.writeME0(u(), A); setU(uint16_t(u() + 1)); return 6;
        case 0x43: bus.writeME0(x(), A); setX(uint16_t(x() - 1)); return 6;
        case 0x53: bus.writeME0(y(), A); setY(uint16_t(y() - 1)); return 6;
        case 0x63: bus.writeME0(u(), A); setU(uint16_t(u() - 1)); return 6;
        case 0x45: A = bus.readME0(x()); setZFlagFrom(A); setX(uint16_t(x() + 1)); return 6;
        case 0x55: A = bus.readME0(y()); setZFlagFrom(A); setY(uint16_t(y() + 1)); return 6;
        case 0x65: A = bus.readME0(u()); setZFlagFrom(A); setU(uint16_t(u() + 1)); return 6;
        case 0x47: A = bus.readME0(x()); setZFlagFrom(A); setX(uint16_t(x() - 1)); return 6;
        case 0x57: A = bus.readME0(y()); setZFlagFrom(A); setY(uint16_t(y() - 1)); return 6;
        case 0x67: A = bus.readME0(u()); setZFlagFrom(A); setU(uint16_t(u() - 1)); return 6;

        // ── LDI ───────────────────────────────────────────────────────────
        case 0xB5: A = fetch8(); setZFlagFrom(A); return 6;
        case 0x4A: XL = fetch8(); return 6;
        case 0x5A: YL = fetch8(); return 6;
        case 0x6A: UL = fetch8(); return 6;
        // ldi xh/yh/uh,n: 6 cycles per the LH5801 instruction reference.
        case 0x48: XH = fetch8(); return 6;
        case 0x58: YH = fetch8(); return 6;
        case 0x68: UH = fetch8(); return 6;
        case 0xAA: S = fetch16(); return 12;

        // ── ADI ───────────────────────────────────────────────────────────
        case 0xB3: A = aluAdd(A, fetch8(), T & 1); return 7;
        case 0x4F: { uint8_t n = fetch8(); bus.writeME0(x(), aluAdd(bus.readME0(x()), n, 0)); return 13; }
        case 0x5F: { uint8_t n = fetch8(); bus.writeME0(y(), aluAdd(bus.readME0(y()), n, 0)); return 13; }
        case 0x6F: { uint8_t n = fetch8(); bus.writeME0(u(), aluAdd(bus.readME0(u()), n, 0)); return 13; }
        case 0xEF: { uint16_t pp = fetch16(); uint8_t n = fetch8(); bus.writeME0(pp, aluAdd(bus.readME0(pp), n, 0)); return 19; }

        // ── SBI ───────────────────────────────────────────────────────────
        case 0xB1: A = aluSub(A, fetch8(), T & 1); return 7;

        // ── ANI ───────────────────────────────────────────────────────────
        case 0xB9: A &= fetch8(); setZFlagFrom(A); return 7;
        case 0x49: { uint8_t n = fetch8(); uint8_t r = uint8_t(bus.readME0(x()) & n); bus.writeME0(x(), r); setZFlagFrom(r); return 13; }
        case 0x59: { uint8_t n = fetch8(); uint8_t r = uint8_t(bus.readME0(y()) & n); bus.writeME0(y(), r); setZFlagFrom(r); return 13; }
        case 0x69: { uint8_t n = fetch8(); uint8_t r = uint8_t(bus.readME0(u()) & n); bus.writeME0(u(), r); setZFlagFrom(r); return 13; }
        case 0xE9: { uint16_t pp = fetch16(); uint8_t n = fetch8(); uint8_t r = uint8_t(bus.readME0(pp) & n); bus.writeME0(pp, r); setZFlagFrom(r); return 19; }

        // ── ORI ───────────────────────────────────────────────────────────
        case 0xBB: A |= fetch8(); setZFlagFrom(A); return 7;
        case 0x4B: { uint8_t n = fetch8(); uint8_t r = uint8_t(bus.readME0(x()) | n); bus.writeME0(x(), r); setZFlagFrom(r); return 13; }
        case 0x5B: { uint8_t n = fetch8(); uint8_t r = uint8_t(bus.readME0(y()) | n); bus.writeME0(y(), r); setZFlagFrom(r); return 13; }
        case 0x6B: { uint8_t n = fetch8(); uint8_t r = uint8_t(bus.readME0(u()) | n); bus.writeME0(u(), r); setZFlagFrom(r); return 13; }
        case 0xEB: { uint16_t pp = fetch16(); uint8_t n = fetch8(); uint8_t r = uint8_t(bus.readME0(pp) | n); bus.writeME0(pp, r); setZFlagFrom(r); return 19; }

        // ── EAI ───────────────────────────────────────────────────────────
        case 0xBD: A ^= fetch8(); setZFlagFrom(A); return 7;

        // ── INC / DEC ────────────────────────────────────────────────────
        case 0xDD: A = aluAdd(A, 1, 0); return 5;
        case 0xDF: A = aluSub(A, 1, 1); return 5;
        case 0x40: XL = aluAdd(XL, 1, 0); return 5;
        case 0x50: YL = aluAdd(YL, 1, 0); return 5;
        case 0x60: UL = aluAdd(UL, 1, 0); return 5;
        case 0x42: XL = aluSub(XL, 1, 1); return 5;
        case 0x52: YL = aluSub(YL, 1, 1); return 5;
        case 0x62: UL = aluSub(UL, 1, 1); return 5;
        case 0x44: setX(uint16_t(x() + 1)); return 5;
        case 0x54: setY(uint16_t(y() + 1)); return 5;
        case 0x64: setU(uint16_t(u() + 1)); return 5;
        case 0x46: setX(uint16_t(x() - 1)); return 5;
        case 0x56: setY(uint16_t(y() - 1)); return 5;
        case 0x66: setU(uint16_t(u() - 1)); return 5;

        // ── CPI ───────────────────────────────────────────────────────────
        case 0xB7: aluSub(A, fetch8(), 1); return 7;
        case 0x4E: aluSub(XL, fetch8(), 1); return 7;
        case 0x5E: aluSub(YL, fetch8(), 1); return 7;
        case 0x6E: aluSub(UL, fetch8(), 1); return 7;
        case 0x4C: aluSub(XH, fetch8(), 1); return 7;
        case 0x5C: aluSub(YH, fetch8(), 1); return 7;
        case 0x6C: aluSub(UH, fetch8(), 1); return 7;

        // ── BII ───────────────────────────────────────────────────────────
        case 0xBF: { uint8_t n = fetch8(); setZFlagFrom(uint8_t(A & n)); return 7; }
        case 0x4D: { uint8_t n = fetch8(); setZFlagFrom(uint8_t(bus.readME0(x()) & n)); return 10; }
        case 0x5D: { uint8_t n = fetch8(); setZFlagFrom(uint8_t(bus.readME0(y()) & n)); return 10; }
        case 0x6D: { uint8_t n = fetch8(); setZFlagFrom(uint8_t(bus.readME0(u()) & n)); return 10; }
        case 0xED: { uint16_t pp = fetch16(); uint8_t n = fetch8(); setZFlagFrom(uint8_t(bus.readME0(pp) & n)); return 16; }

        // ── TIN / CIN ────────────────────────────────────────────────────
        case 0xF5: { uint8_t v = bus.readME0(x()); bus.writeME0(y(), v); setX(uint16_t(x() + 1)); setY(uint16_t(y() + 1)); return 7; }
        case 0xF7: { aluSub(A, bus.readME0(x()), 1); setX(uint16_t(x() + 1)); return 7; }

        // ── Rotate / shift / digit-rotate / nibble-swap ─────────────────
        // ROL/SHL also set H/Z besides C; ROR/SHR force V=0 but still set
        // H/Z.
        case 0xDB: { // ROL
            bool oldBit7 = (A & 0x80) != 0;
            A = uint8_t((A << 1) | ((T & 1) ? 1 : 0));
            setFlagBit(0x01, oldBit7);
            setFlagBit(0x10, (A & 0x10) != 0);
            setFlagBit(0x08, oldBit7 != ((A & 0x80) != 0));
            setZFlagFrom(A);
            return 8;
        }
        case 0xD1: { // ROR
            bool oldBit0 = (A & 0x01) != 0;
            A = uint8_t((A >> 1) | ((T & 1) ? 0x80 : 0));
            setFlagBit(0x01, oldBit0);
            setFlagBit(0x10, (A & 0x08) != 0);
            setFlagBit(0x08, false);
            setZFlagFrom(A);
            return 9;
        }
        case 0xD9: { // SHL
            bool oldBit7 = (A & 0x80) != 0;
            A = uint8_t(A << 1);
            setFlagBit(0x01, oldBit7);
            setFlagBit(0x10, (A & 0x10) != 0);
            setFlagBit(0x08, oldBit7 != ((A & 0x80) != 0));
            setZFlagFrom(A);
            return 6;
        }
        case 0xD5: { // SHR
            bool oldBit0 = (A & 0x01) != 0;
            A = uint8_t(A >> 1);
            setFlagBit(0x01, oldBit0);
            setFlagBit(0x10, (A & 0x08) != 0);
            setFlagBit(0x08, false);
            setZFlagFrom(A);
            return 9;
        }
        // DRL/DRR: A becomes the *entire* old memory byte (not just one
        // nibble); memory becomes a merge of its own old low nibble with
        // A's old opposite nibble. A's other old nibble is discarded, not
        // conserved anywhere.
        case 0xD7: { // drl (x)
            uint8_t memOld = bus.readME0(x());
            uint8_t memNew = drlMerge(memOld, A);
            A = memOld;
            bus.writeME0(x(), memNew);
            return 12;
        }
        case 0xD3: { // drr (x)
            uint8_t memOld = bus.readME0(x());
            uint8_t memNew = drrMerge(memOld, A);
            A = memOld;
            bus.writeME0(x(), memNew);
            return 12;
        }
        case 0xF1: A = uint8_t(((A & 0x0F) << 4) | ((A >> 4) & 0x0F)); return 6;

        // ── CPU control ──────────────────────────────────────────────────
        case 0xFB: setFlagBit(0x01, true); return 4;
        case 0xF9: setFlagBit(0x01, false); return 4;
        case 0xE1: PU = true; return 4;
        case 0xE3: PU = false; return 4;
        case 0xA8: PV = true; return 4;
        case 0xB8: PV = false; return 4;
        case 0x38: return 5; // NOP (== sta VH, no observable effect)

        // ── Jumps / branches ─────────────────────────────────────────────
        case 0xBA: P = fetch16(); return 12;
        case 0x8E: doBranch(true, fetch8()); return 8;
        case 0x9E: doBranch(false, fetch8()); return 9;
        case 0x83: { uint8_t e = fetch8(); if (flagC()) { doBranch(true, e); return 10; } return 8; }
        case 0x93: { uint8_t e = fetch8(); if (flagC()) { doBranch(false, e); return 11; } return 8; }
        case 0x81: { uint8_t e = fetch8(); if (!flagC()) { doBranch(true, e); return 10; } return 8; }
        case 0x91: { uint8_t e = fetch8(); if (!flagC()) { doBranch(false, e); return 11; } return 8; }
        case 0x87: { uint8_t e = fetch8(); if (flagH()) { doBranch(true, e); return 10; } return 8; }
        case 0x97: { uint8_t e = fetch8(); if (flagH()) { doBranch(false, e); return 11; } return 8; }
        case 0x85: { uint8_t e = fetch8(); if (!flagH()) { doBranch(true, e); return 10; } return 8; }
        case 0x95: { uint8_t e = fetch8(); if (!flagH()) { doBranch(false, e); return 11; } return 8; }
        case 0x8B: { uint8_t e = fetch8(); if (flagZ()) { doBranch(true, e); return 10; } return 8; }
        case 0x9B: { uint8_t e = fetch8(); if (flagZ()) { doBranch(false, e); return 11; } return 8; }
        case 0x89: { uint8_t e = fetch8(); if (!flagZ()) { doBranch(true, e); return 10; } return 8; }
        case 0x99: { uint8_t e = fetch8(); if (!flagZ()) { doBranch(false, e); return 11; } return 8; }
        case 0x8F: { uint8_t e = fetch8(); if (flagV()) { doBranch(true, e); return 10; } return 8; }
        case 0x9F: { uint8_t e = fetch8(); if (flagV()) { doBranch(false, e); return 11; } return 8; }
        case 0x8D: { uint8_t e = fetch8(); if (!flagV()) { doBranch(true, e); return 10; } return 8; }
        case 0x9D: { uint8_t e = fetch8(); if (!flagV()) { doBranch(false, e); return 11; } return 8; }
        case 0x88: { // lop ul,e
            uint8_t e = fetch8();
            bool noBorrow = UL != 0;
            UL = uint8_t(UL - 1);
            if (noBorrow) { P = uint16_t(P - e); return 11; }
            return 8;
        }

        // ── Subroutine / vector calls ────────────────────────────────────
        case 0xBE: { uint16_t pp = fetch16(); push16(P); P = pp; return 19; }
        case 0xCD: { uint8_t idx = fetch8(); vectorCall(idx); return 20; }
        case 0xC3: { uint8_t idx = fetch8(); if (flagC()) { vectorCall(idx); return 21; } return 8; }
        case 0xC1: { uint8_t idx = fetch8(); if (!flagC()) { vectorCall(idx); return 21; } return 8; }
        case 0xC7: { uint8_t idx = fetch8(); if (flagH()) { vectorCall(idx); return 21; } return 8; }
        case 0xC5: { uint8_t idx = fetch8(); if (!flagH()) { vectorCall(idx); return 21; } return 8; }
        case 0xCB: { uint8_t idx = fetch8(); if (flagZ()) { vectorCall(idx); return 21; } return 8; }
        case 0xC9: { uint8_t idx = fetch8(); if (!flagZ()) { vectorCall(idx); return 21; } return 8; }
        case 0xCF: { uint8_t idx = fetch8(); if (flagV()) { vectorCall(idx); return 21; } return 8; }

        // ── Return ────────────────────────────────────────────────────────
        case 0x9A: P = pop16(); return 11;
        case 0x8A: { P = pop16(); T = popByte() & 0x1F; return 14; }

        default:
            if (op >= 0xC0 && (op & 0x01) == 0) {
                // vej (nn): 1-byte vector call, opcode IS the vector index
                vectorCall(op);
                return 17;
            }
            // Undocumented/unimplemented opcode: signal it to step() (see
            // kIllegalOpcode) rather than silently behaving like a real
            // instruction — step() still lets execution continue (a
            // malformed stream shouldn't wedge forever), but the event is
            // now distinguishable via consumeIllegalOpcodeHit() instead of
            // being indistinguishable from a genuinely emulated NOP.
            return kIllegalOpcode;
    }
}

// ── FD-prefixed (two-byte) opcode dispatch ──────────────────────────────────

int LH5801::executeFD(uint8_t op) {
    switch (op) {
        // ── ADC / SBC / AND / ORA / EOR / CPA / BIT — ME1 (Rreg) and (pp) ──
        case 0x03: A = aluAdd(A, bus.readME1(x()), T & 1); return 11;
        case 0x13: A = aluAdd(A, bus.readME1(y()), T & 1); return 11;
        case 0x23: A = aluAdd(A, bus.readME1(u()), T & 1); return 11;
        case 0xA3: { uint16_t pp = fetch16(); A = aluAdd(A, bus.readME1(pp), T & 1); return 17; }
        // sbc #(x)/#(y)/#(u): 11 cycles, matching every other ME1 (Rreg)
        // form.
        case 0x01: A = aluSub(A, bus.readME1(x()), T & 1); return 11;
        case 0x11: A = aluSub(A, bus.readME1(y()), T & 1); return 11;
        case 0x21: A = aluSub(A, bus.readME1(u()), T & 1); return 11;
        case 0xA1: { uint16_t pp = fetch16(); A = aluSub(A, bus.readME1(pp), T & 1); return 17; }
        case 0x09: A &= bus.readME1(x()); setZFlagFrom(A); return 11;
        case 0x19: A &= bus.readME1(y()); setZFlagFrom(A); return 11;
        case 0x29: A &= bus.readME1(u()); setZFlagFrom(A); return 11;
        case 0xA9: { uint16_t pp = fetch16(); A &= bus.readME1(pp); setZFlagFrom(A); return 17; }
        case 0x0B: A |= bus.readME1(x()); setZFlagFrom(A); return 11;
        case 0x1B: A |= bus.readME1(y()); setZFlagFrom(A); return 11;
        case 0x2B: A |= bus.readME1(u()); setZFlagFrom(A); return 11;
        case 0xAB: { uint16_t pp = fetch16(); A |= bus.readME1(pp); setZFlagFrom(A); return 17; }
        case 0x0D: A ^= bus.readME1(x()); setZFlagFrom(A); return 11;
        case 0x1D: A ^= bus.readME1(y()); setZFlagFrom(A); return 11;
        case 0x2D: A ^= bus.readME1(u()); setZFlagFrom(A); return 11;
        case 0xAD: { uint16_t pp = fetch16(); A ^= bus.readME1(pp); setZFlagFrom(A); return 17; }
        case 0x07: aluSub(A, bus.readME1(x()), 1); return 11;
        case 0x17: aluSub(A, bus.readME1(y()), 1); return 11;
        case 0x27: aluSub(A, bus.readME1(u()), 1); return 11;
        case 0xA7: { uint16_t pp = fetch16(); aluSub(A, bus.readME1(pp), 1); return 17; }
        case 0x0F: setZFlagFrom(uint8_t(A & bus.readME1(x()))); return 11;
        case 0x1F: setZFlagFrom(uint8_t(A & bus.readME1(y()))); return 11;
        case 0x2F: setZFlagFrom(uint8_t(A & bus.readME1(u()))); return 11;
        case 0xAF: { uint16_t pp = fetch16(); setZFlagFrom(uint8_t(A & bus.readME1(pp))); return 17; }

        // ── LDA / STA — ME1 ────────────────────────────────────────────────
        case 0x05: A = bus.readME1(x()); setZFlagFrom(A); return 10;
        case 0x15: A = bus.readME1(y()); setZFlagFrom(A); return 10;
        case 0x25: A = bus.readME1(u()); setZFlagFrom(A); return 10;
        case 0xA5: { uint16_t pp = fetch16(); A = bus.readME1(pp); setZFlagFrom(A); return 16; }
        case 0x0E: bus.writeME1(x(), A); return 10;
        case 0x1E: bus.writeME1(y(), A); return 10;
        case 0x2E: bus.writeME1(u(), A); return 10;
        case 0xAE: { uint16_t pp = fetch16(); bus.writeME1(pp, A); return 16; }

        // ── ADI / ANI / ORI / BII — ME1 ────────────────────────────────────
        case 0x4F: { uint8_t n = fetch8(); bus.writeME1(x(), aluAdd(bus.readME1(x()), n, 0)); return 17; }
        case 0x5F: { uint8_t n = fetch8(); bus.writeME1(y(), aluAdd(bus.readME1(y()), n, 0)); return 17; }
        case 0x6F: { uint8_t n = fetch8(); bus.writeME1(u(), aluAdd(bus.readME1(u()), n, 0)); return 17; }
        case 0xEF: { uint16_t pp = fetch16(); uint8_t n = fetch8(); bus.writeME1(pp, aluAdd(bus.readME1(pp), n, 0)); return 23; }
        case 0x49: { uint8_t n = fetch8(); uint8_t r = uint8_t(bus.readME1(x()) & n); bus.writeME1(x(), r); setZFlagFrom(r); return 17; }
        case 0x59: { uint8_t n = fetch8(); uint8_t r = uint8_t(bus.readME1(y()) & n); bus.writeME1(y(), r); setZFlagFrom(r); return 17; }
        case 0x69: { uint8_t n = fetch8(); uint8_t r = uint8_t(bus.readME1(u()) & n); bus.writeME1(u(), r); setZFlagFrom(r); return 17; }
        case 0xE9: { uint16_t pp = fetch16(); uint8_t n = fetch8(); uint8_t r = uint8_t(bus.readME1(pp) & n); bus.writeME1(pp, r); setZFlagFrom(r); return 23; }
        case 0x4B: { uint8_t n = fetch8(); uint8_t r = uint8_t(bus.readME1(x()) | n); bus.writeME1(x(), r); setZFlagFrom(r); return 17; }
        case 0x5B: { uint8_t n = fetch8(); uint8_t r = uint8_t(bus.readME1(y()) | n); bus.writeME1(y(), r); setZFlagFrom(r); return 17; }
        case 0x6B: { uint8_t n = fetch8(); uint8_t r = uint8_t(bus.readME1(u()) | n); bus.writeME1(u(), r); setZFlagFrom(r); return 17; }
        case 0xEB: { uint16_t pp = fetch16(); uint8_t n = fetch8(); uint8_t r = uint8_t(bus.readME1(pp) | n); bus.writeME1(pp, r); setZFlagFrom(r); return 23; }
        case 0x4D: { uint8_t n = fetch8(); setZFlagFrom(uint8_t(bus.readME1(x()) & n)); return 14; }
        case 0x5D: { uint8_t n = fetch8(); setZFlagFrom(uint8_t(bus.readME1(y()) & n)); return 14; }
        case 0x6D: { uint8_t n = fetch8(); setZFlagFrom(uint8_t(bus.readME1(u()) & n)); return 14; }
        case 0xED: { uint16_t pp = fetch16(); uint8_t n = fetch8(); setZFlagFrom(uint8_t(bus.readME1(pp) & n)); return 20; }

        // ── DCA / DCS — ME1 ────────────────────────────────────────────────
        case 0x8C: A = bcdAdd(A, bus.readME1(x()), T & 1); return 19;
        case 0x9C: A = bcdAdd(A, bus.readME1(y()), T & 1); return 19;
        case 0xAC: A = bcdAdd(A, bus.readME1(u()), T & 1); return 19;
        case 0x0C: A = bcdSub(A, bus.readME1(x()), T & 1); return 17;
        case 0x1C: A = bcdSub(A, bus.readME1(y()), T & 1); return 17;
        case 0x2C: A = bcdSub(A, bus.readME1(u()), T & 1); return 17;

        // ── ADR (16-bit: Rreg = Rreg + A) ───────────────────────────────────
        //
        // ADR leaves C/V/H/Z *unchanged* on real silicon -- confirmed on a
        // real PC-1500A (via examples/debug/adrtest_1500a.asm): carry
        // survives an ADR intact. This is despite the PC-1500 Technical
        // Reference Manual's §2-4 prose ("C, H, Z, and V may change") and
        // worked example, which describe the low-byte addition's flags as
        // published to the status register -- do not "correct" this back
        // to match the manual's prose.
        //
        // The ROM's key-dispatch path depends on this: carry must survive
        // an `adr y` at 0xD2AC, across the `rtn` at 0xD2AE, to reach the
        // `bcr` at 0xDCA0. With the flags clobbered that branch goes the
        // wrong way and the PRO-mode line-listing / BREAK-peek redraw never
        // runs -- display RAM stays stale even though the ROM's own text
        // buffer updates.
        //
        // aluAdd() unavoidably writes the flags as a side effect, so T is
        // saved and restored around it here to preserve the caller's carry.
        case 0xCA: case 0xDA: case 0xEA: {
            int pair = op == 0xCA ? 0 : (op == 0xDA ? 1 : 2);
            uint8_t savedFlags = T;
            uint8_t rl = lowOf(pair);
            uint8_t result = aluAdd(rl, A, 0);
            setLowOf(pair, result);
            if (flagC()) setHighOf(pair, uint8_t(highOf(pair) + 1));
            T = savedFlags;
            return 11;
        }

        // ── INC / DEC (high byte) ───────────────────────────────────────────
        case 0x40: XH = aluAdd(XH, 1, 0); return 9;
        case 0x50: YH = aluAdd(YH, 1, 0); return 9;
        case 0x60: UH = aluAdd(UH, 1, 0); return 9;
        case 0x42: XH = aluSub(XH, 1, 1); return 9;
        case 0x52: YH = aluSub(YH, 1, 1); return 9;
        case 0x62: UH = aluSub(UH, 1, 1); return 9;

        // ── LDX / STX ────────────────────────────────────────────────────
        case 0x18: setX(y()); return 11;
        case 0x28: setX(u()); return 11;
        case 0x48: setX(S); return 11;
        case 0x58: setX(P); return 11;
        case 0x5A: setY(x()); return 11;
        case 0x6A: setU(x()); return 11;
        case 0x4E: S = x(); return 11;
        case 0x5E: P = x(); return 11;

        // ── PSH / POP ────────────────────────────────────────────────────
        case 0xC8: pushByte(A); return 11;
        case 0x88: push16(x()); return 14;
        case 0x98: push16(y()); return 14;
        case 0xA8: push16(u()); return 14;
        case 0x8A: A = popByte(); setZFlagFrom(A); return 12;
        case 0x0A: setX(pop16()); return 15;
        case 0x1A: setY(pop16()); return 15;
        case 0x2A: setU(pop16()); return 15;

        // ── ATT / TTA ────────────────────────────────────────────────────
        case 0xEC: T = A & 0x1F; return 9;
        case 0xAA: A = T; setZFlagFrom(A); return 9;

        // ── DRL / DRR — ME1 (see the ME0 forms' comment for provenance) ──
        case 0xD7: {
            uint8_t memOld = bus.readME1(x());
            uint8_t memNew = drlMerge(memOld, A);
            A = memOld;
            bus.writeME1(x(), memNew);
            return 16;
        }
        case 0xD3: {
            uint8_t memOld = bus.readME1(x());
            uint8_t memNew = drrMerge(memOld, A);
            A = memOld;
            bus.writeME1(x(), memNew);
            return 16;
        }

        // ── CPU control / I/O ────────────────────────────────────────────
        case 0x81: setFlagBit(0x02, true); return 8;
        case 0xBE: setFlagBit(0x02, false); return 8;
        case 0xC1: DISP = true; bus.setDisplayEnabled(true); return 9; // 9 cycles per the LH5801 instruction reference
        case 0xC0: DISP = false; bus.setDisplayEnabled(false); return 8;
        case 0xCE: setTimer(A); return 9; // AM0: bit8 forced 0 (A alone, since A is 8-bit)
        case 0xDE: setTimer(uint16_t(A | 0x100)); return 9;
        case 0xCC: bus.writeOutputPort(A); return 9;
        case 0xBA: A = bus.readInputPort(); setZFlagFrom(A); return 9;
        case 0x8E: return 8; // cdv — clock divider not modeled
        case 0xB1: m_halted = true; return 9;
        case 0x4C: m_poweredOff = true; return 8; // off -- BF flip-flop reset, real power-down (see poweredOff())

        default:
            return kIllegalOpcode; // undocumented FD-prefixed opcode — see execute()'s default case
    }
}
