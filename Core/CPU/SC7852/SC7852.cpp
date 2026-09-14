#include "SC7852.hpp"

#include <algorithm>

// Standard Zilog Z-80A instruction set/timing. T-state (cycle) counts
// follow the documented nominal values; "No extra wait state" in
// SC7852.hpp's class comment covers the one open timing question (extra
// wait states).

namespace {
constexpr uint8_t kFlagC  = 0x01;
constexpr uint8_t kFlagN  = 0x02;
constexpr uint8_t kFlagPV = 0x04;
constexpr uint8_t kFlagX  = 0x08; // undocumented, mirrors bit 3 of result
constexpr uint8_t kFlagH  = 0x10;
constexpr uint8_t kFlagY  = 0x20; // undocumented, mirrors bit 5 of result
constexpr uint8_t kFlagZ  = 0x40;
constexpr uint8_t kFlagS  = 0x80;
} // namespace

SC7852::SC7852(SC7852Bus& busRef) : bus(busRef) {
    reset();
}

void SC7852::reset() {
    PC = 0x0000;
    SP = 0xFFFF;
    I = 0; R = 0;
    IFF1 = false; IFF2 = false;
    IM = 0;
    m_halted = false;
    m_irqPending = false;
    m_nmiPending = false;
    // A/F and the general-purpose registers are left as their construction-
    // time values on a real Z-80 reset (undefined/whatever they were) —
    // this core initializes them to 0xFF/0 at construction and never
    // re-randomizes on reset(), which is a deliberate, deterministic
    // simplification for reproducible tests, not a claim about real
    // silicon's power-up RAM pattern.
}

// ── Fetch helpers ─────────────────────────────────────────────────────────

uint8_t SC7852::fetch8() {
    uint8_t v = bus.readMem(PC++);
    bumpR();
    return v;
}

uint16_t SC7852::fetch16() {
    uint8_t lo = bus.readMem(PC++);
    uint8_t hi = bus.readMem(PC++);
    bumpR();
    return uint16_t(lo | (hi << 8));
}

// ── Flags ──────────────────────────────────────────────────────────────

bool SC7852::parity(uint8_t v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (v & 1) == 0;
}

void SC7852::setSZ53(uint8_t result) {
    setFlagBit(kFlagS, (result & 0x80) != 0);
    setFlagBit(kFlagZ, result == 0);
    setFlagBit(kFlagY, (result & kFlagY) != 0);
    setFlagBit(kFlagX, (result & kFlagX) != 0);
}

void SC7852::setSZP53(uint8_t result) {
    setSZ53(result);
    setFlagBit(kFlagPV, parity(result));
}

uint8_t SC7852::aluAdd8(uint8_t a, uint8_t operand, uint8_t carryIn, bool isSub) {
    uint8_t op = isSub ? uint8_t(~operand) : operand;
    uint8_t cin = isSub ? uint8_t(1 - carryIn) : carryIn;
    // For subtraction, a - operand - borrowIn == a + ~operand + (1-borrowIn).
    unsigned full = unsigned(a) + unsigned(op) + cin;
    uint8_t result = uint8_t(full);
    bool carryOut = full > 0xFF;
    // H is the borrow out of bit 3 for SUB, the carry out of bit 3 for ADD.
    // The add-the-complement trick above makes `carryOut` come out inverted
    // for SUB (hence the `!carryOut` on kFlagC below), so halfCarry needs
    // the same inversion for SUB (computed directly below via a signed
    // nibble subtraction, rather than via the complement trick) so that
    // DAA -- which consumes H -- decimal-adjusts BCD subtraction correctly.
    bool halfCarry = isSub
        ? ((int(a & 0x0F) - int(operand & 0x0F) - int(carryIn)) < 0)
        : (((a & 0x0F) + (op & 0x0F) + cin) > 0x0F);
    bool overflow = ((a ^ result) & (op ^ result) & 0x80) != 0;
    setSZ53(result);
    setFlagBit(kFlagH, halfCarry);
    setFlagBit(kFlagPV, overflow);
    setFlagBit(kFlagN, isSub);
    setFlagBit(kFlagC, isSub ? !carryOut : carryOut);
    return result;
}

void SC7852::cp8(uint8_t a, uint8_t operand) {
    uint8_t saved = A;
    (void)aluAdd8(a, operand, 0, true);
    // CP sets flags like SUB but discards the result and restores the
    // undocumented Y/X bits from the *operand*, not the (discarded)
    // result -- standard documented Z-80 CP quirk.
    setFlagBit(kFlagY, (operand & kFlagY) != 0);
    setFlagBit(kFlagX, (operand & kFlagX) != 0);
    A = saved;
}

uint8_t SC7852::inc8(uint8_t v) {
    uint8_t result = uint8_t(v + 1);
    setSZ53(result);
    setFlagBit(kFlagH, (v & 0x0F) == 0x0F);
    setFlagBit(kFlagPV, v == 0x7F);
    setFlagBit(kFlagN, false);
    return result;
}

uint8_t SC7852::dec8(uint8_t v) {
    uint8_t result = uint8_t(v - 1);
    setSZ53(result);
    setFlagBit(kFlagH, (v & 0x0F) == 0x00);
    setFlagBit(kFlagPV, v == 0x80);
    setFlagBit(kFlagN, true);
    return result;
}

void SC7852::and8(uint8_t operand) {
    A &= operand;
    setSZP53(A);
    setFlagBit(kFlagH, true);
    setFlagBit(kFlagN, false);
    setFlagBit(kFlagC, false);
}

void SC7852::or8(uint8_t operand) {
    A |= operand;
    setSZP53(A);
    setFlagBit(kFlagH, false);
    setFlagBit(kFlagN, false);
    setFlagBit(kFlagC, false);
}

void SC7852::xor8(uint8_t operand) {
    A ^= operand;
    setSZP53(A);
    setFlagBit(kFlagH, false);
    setFlagBit(kFlagN, false);
    setFlagBit(kFlagC, false);
}

uint16_t SC7852::addHL16(uint16_t hlv, uint16_t operand) {
    unsigned full = unsigned(hlv) + unsigned(operand);
    uint16_t result = uint16_t(full);
    setFlagBit(kFlagH, ((hlv & 0x0FFF) + (operand & 0x0FFF)) > 0x0FFF);
    setFlagBit(kFlagN, false);
    setFlagBit(kFlagC, full > 0xFFFF);
    setFlagBit(kFlagY, (uint8_t(result >> 8) & kFlagY) != 0);
    setFlagBit(kFlagX, (uint8_t(result >> 8) & kFlagX) != 0);
    return result;
}

uint16_t SC7852::adc16(uint16_t hlv, uint16_t operand) {
    unsigned cin = F & kFlagC ? 1u : 0u;
    unsigned full = unsigned(hlv) + unsigned(operand) + cin;
    uint16_t result = uint16_t(full);
    bool halfCarry = ((hlv & 0x0FFF) + (operand & 0x0FFF) + cin) > 0x0FFF;
    bool overflow = ((hlv ^ result) & (operand ^ result) & 0x8000) != 0;
    setFlagBit(kFlagS, (result & 0x8000) != 0);
    setFlagBit(kFlagZ, result == 0);
    setFlagBit(kFlagY, (uint8_t(result >> 8) & kFlagY) != 0);
    setFlagBit(kFlagX, (uint8_t(result >> 8) & kFlagX) != 0);
    setFlagBit(kFlagH, halfCarry);
    setFlagBit(kFlagPV, overflow);
    setFlagBit(kFlagN, false);
    setFlagBit(kFlagC, full > 0xFFFF);
    return result;
}

uint16_t SC7852::sbc16(uint16_t hlv, uint16_t operand) {
    unsigned cin = F & kFlagC ? 1u : 0u;
    unsigned full = unsigned(hlv) - unsigned(operand) - cin;
    uint16_t result = uint16_t(full);
    bool borrow = (unsigned(hlv) < unsigned(operand) + cin);
    bool halfBorrow = (int(hlv & 0x0FFF) - int(operand & 0x0FFF) - int(cin)) < 0;
    bool overflow = ((hlv ^ operand) & (hlv ^ result) & 0x8000) != 0;
    setFlagBit(kFlagS, (result & 0x8000) != 0);
    setFlagBit(kFlagZ, result == 0);
    setFlagBit(kFlagY, (uint8_t(result >> 8) & kFlagY) != 0);
    setFlagBit(kFlagX, (uint8_t(result >> 8) & kFlagX) != 0);
    setFlagBit(kFlagH, halfBorrow);
    setFlagBit(kFlagPV, overflow);
    setFlagBit(kFlagN, true);
    setFlagBit(kFlagC, borrow);
    return result;
}

void SC7852::daa() {
    // Standard Z-80 DAA: the 0x06 and 0x60 correction conditions are the
    // SAME regardless of whether the preceding op was an ADD or a SUB --
    // only the direction (A += / A -=) depends on N. This matters for BCD
    // *subtraction*, which the PC-1600 ROM's floating-point add uses
    // whenever the operands have opposite signs (e.g. `L + 5` with L = -5).
    const bool sub = (F & kFlagN) != 0;
    const uint8_t oldA = A;
    uint8_t a = A;
    if ((F & kFlagH) || (oldA & 0x0F) > 9) a = uint8_t(sub ? a - 0x06 : a + 0x06);
    if ((F & kFlagC) || oldA > 0x99)        a = uint8_t(sub ? a - 0x60 : a + 0x60);

    A = a;
    setSZP53(A);                                  // S, Z, PV(parity), X, Y from A
    setFlagBit(kFlagH, ((oldA ^ a) & kFlagH) != 0);
    setFlagBit(kFlagC, (F & kFlagC) != 0 || oldA > 0x99);
    // N is left unchanged by DAA.
}

// ── Rotate/shift ───────────────────────────────────────────────────────

uint8_t SC7852::rlc(uint8_t v) {
    bool carry = (v & 0x80) != 0;
    uint8_t result = uint8_t((v << 1) | (carry ? 1 : 0));
    setSZP53(result);
    setFlagBit(kFlagH, false);
    setFlagBit(kFlagN, false);
    setFlagBit(kFlagC, carry);
    return result;
}
uint8_t SC7852::rrc(uint8_t v) {
    bool carry = (v & 0x01) != 0;
    uint8_t result = uint8_t((v >> 1) | (carry ? 0x80 : 0));
    setSZP53(result);
    setFlagBit(kFlagH, false);
    setFlagBit(kFlagN, false);
    setFlagBit(kFlagC, carry);
    return result;
}
uint8_t SC7852::rl(uint8_t v) {
    bool cin = (F & kFlagC) != 0;
    bool carry = (v & 0x80) != 0;
    uint8_t result = uint8_t((v << 1) | (cin ? 1 : 0));
    setSZP53(result);
    setFlagBit(kFlagH, false);
    setFlagBit(kFlagN, false);
    setFlagBit(kFlagC, carry);
    return result;
}
uint8_t SC7852::rr(uint8_t v) {
    bool cin = (F & kFlagC) != 0;
    bool carry = (v & 0x01) != 0;
    uint8_t result = uint8_t((v >> 1) | (cin ? 0x80 : 0));
    setSZP53(result);
    setFlagBit(kFlagH, false);
    setFlagBit(kFlagN, false);
    setFlagBit(kFlagC, carry);
    return result;
}
uint8_t SC7852::sla(uint8_t v) {
    bool carry = (v & 0x80) != 0;
    uint8_t result = uint8_t(v << 1);
    setSZP53(result);
    setFlagBit(kFlagH, false);
    setFlagBit(kFlagN, false);
    setFlagBit(kFlagC, carry);
    return result;
}
uint8_t SC7852::sra(uint8_t v) {
    bool carry = (v & 0x01) != 0;
    uint8_t result = uint8_t((v >> 1) | (v & 0x80));
    setSZP53(result);
    setFlagBit(kFlagH, false);
    setFlagBit(kFlagN, false);
    setFlagBit(kFlagC, carry);
    return result;
}
uint8_t SC7852::sll(uint8_t v) { // undocumented "SLL"/"SL1" -- shifts in a 1
    bool carry = (v & 0x80) != 0;
    uint8_t result = uint8_t((v << 1) | 1);
    setSZP53(result);
    setFlagBit(kFlagH, false);
    setFlagBit(kFlagN, false);
    setFlagBit(kFlagC, carry);
    return result;
}
uint8_t SC7852::srl(uint8_t v) {
    bool carry = (v & 0x01) != 0;
    uint8_t result = uint8_t(v >> 1);
    setSZP53(result);
    setFlagBit(kFlagH, false);
    setFlagBit(kFlagN, false);
    setFlagBit(kFlagC, carry);
    return result;
}

// ── Stack ──────────────────────────────────────────────────────────────

void SC7852::pushWord(uint16_t v) {
    bus.writeMem(uint16_t(SP - 1), uint8_t(v >> 8));
    bus.writeMem(uint16_t(SP - 2), uint8_t(v));
    SP = uint16_t(SP - 2);
}
uint16_t SC7852::popWord() {
    uint8_t lo = bus.readMem(SP);
    uint8_t hi = bus.readMem(uint16_t(SP + 1));
    SP = uint16_t(SP + 2);
    return uint16_t(lo | (hi << 8));
}

// ── Register-pair table helpers ───────────────────────────────────────

uint8_t SC7852::readReg8(int code) {
    switch (code) {
        case 0: return B;
        case 1: return C;
        case 2: return D;
        case 3: return E;
        case 4: return H;
        case 5: return L;
        case 6: return bus.readMem(hl());
        default: return A;
    }
}
void SC7852::writeReg8(int code, uint8_t v) {
    switch (code) {
        case 0: B = v; break;
        case 1: C = v; break;
        case 2: D = v; break;
        case 3: E = v; break;
        case 4: H = v; break;
        case 5: L = v; break;
        case 6: bus.writeMem(hl(), v); break;
        default: A = v; break;
    }
}
uint16_t SC7852::readReg16_sp(int code) const {
    switch (code) {
        case 0: return uint16_t(B << 8) | C;
        case 1: return uint16_t(D << 8) | E;
        case 2: return uint16_t(H << 8) | L;
        default: return SP;
    }
}
void SC7852::writeReg16_sp(int code, uint16_t v) {
    switch (code) {
        case 0: B = uint8_t(v >> 8); C = uint8_t(v); break;
        case 1: D = uint8_t(v >> 8); E = uint8_t(v); break;
        case 2: H = uint8_t(v >> 8); L = uint8_t(v); break;
        default: SP = v; break;
    }
}
uint16_t SC7852::readReg16_af(int code) const {
    switch (code) {
        case 0: return uint16_t(B << 8) | C;
        case 1: return uint16_t(D << 8) | E;
        case 2: return uint16_t(H << 8) | L;
        default: return uint16_t(A << 8) | F;
    }
}
void SC7852::writeReg16_af(int code, uint16_t v) {
    switch (code) {
        case 0: B = uint8_t(v >> 8); C = uint8_t(v); break;
        case 1: D = uint8_t(v >> 8); E = uint8_t(v); break;
        case 2: H = uint8_t(v >> 8); L = uint8_t(v); break;
        default: A = uint8_t(v >> 8); F = uint8_t(v); break;
    }
}

bool SC7852::condTrue(int code) const {
    switch (code) {
        case 0: return !(F & kFlagZ);
        case 1: return (F & kFlagZ) != 0;
        case 2: return !(F & kFlagC);
        case 3: return (F & kFlagC) != 0;
        case 4: return !(F & kFlagPV);
        case 5: return (F & kFlagPV) != 0;
        case 6: return !(F & kFlagS);
        default: return (F & kFlagS) != 0;
    }
}

// ── Interrupts ─────────────────────────────────────────────────────────

void SC7852::requestInterrupt() { m_irqPending = true; }
void SC7852::requestNMI() { m_nmiPending = true; }

int SC7852::serviceInterrupt() {
    if (m_nmiPending) {
        m_nmiPending = false;
        m_halted = false;
        IFF2 = IFF1;
        IFF1 = false;
        pushWord(PC);
        PC = 0x0066;
        return 11;
    }
    if (m_irqPending && IFF1) {
        m_irqPending = false;
        m_halted = false;
        IFF1 = false;
        IFF2 = false;
        int cost;
        switch (IM) {
            case 0: // Real IM0 executes whatever instruction the
                    // interrupting device places on the data bus (usually
                    // a single-byte RST); not modeled -- this core treats
                    // IM0 identically to IM1 (RST 38H) since nothing in
                    // this project's scope drives IM0 with a non-RST byte.
            case 1:
                pushWord(PC);
                PC = 0x0038;
                cost = 13;
                break;
            default: { // IM2: vector = (I<<8)|m_im2VectorLow, points at a
                       // 16-bit jump-table entry in memory
                uint16_t vecAddr = uint16_t(uint16_t(I) << 8) | m_im2VectorLow;
                uint8_t lo = bus.readMem(vecAddr);
                uint8_t hi = bus.readMem(uint16_t(vecAddr + 1));
                pushWord(PC);
                PC = uint16_t(lo | (hi << 8));
                cost = 19;
                break;
            }
        }
        return cost;
    }
    if (m_irqPending && m_halted) {
        // HALT always wakes on any interrupt even if IFF1 is clear; the
        // interrupt itself just isn't serviced in that case (standard
        // documented Z-80 behavior). Consume the pending flag either way
        // so a masked IRQ doesn't re-wake indefinitely once already
        // acknowledged as "woke the CPU up."
        m_irqPending = false;
        m_halted = false;
    }
    return -1; // nothing serviced -- step() should fetch/execute normally
}

// ── step() ────────────────────────────────────────────────────────────

int SC7852::step() {
    if (m_irqPending || m_nmiPending) {
        int serviced = serviceInterrupt();
        if (serviced >= 0) return serviced; // interrupt ack consumes this step() call on its own; no trace frame
    }
    if (m_halted) {
        return 0;
    }

    uint32_t tf = traceFlags();
    if (tf & TRACE_BREAKPOINTS) {
        std::lock_guard<std::mutex> lock(m_traceMutex);
        if (std::binary_search(m_breakpoints.begin(), m_breakpoints.end(), PC)) {
            m_breakpointHit = true;
            return 0;
        }
    }

    uint16_t pcAtStart = PC;
    uint8_t opcode = fetch8();
    uint16_t opcodeWord = opcode;
    int cycles;
    // TODO(wait-state-scope): every T-state count returned below is
    // nominal Zilog timing with no extra wait state inserted -- the
    // locked default for the Technical Reference Manual's unresolved "1
    // WAIT automatically inserted in the machine cycle" note (see
    // SC7852.hpp's class comment). Revisit only if a disassembled ROM
    // delay loop or observed real-hardware behavior disagrees.
    switch (opcode) {
        case 0xCB: { uint8_t op2 = fetch8(); opcodeWord = uint16_t(0xCB00 | op2); cycles = 4 + executeCB(op2); break; }
        case 0xED: { uint8_t op2 = fetch8(); opcodeWord = uint16_t(0xED00 | op2); cycles = 4 + executeED(op2); break; }
        case 0xDD: { uint8_t op2 = fetch8(); opcodeWord = uint16_t(0xDD00 | op2); cycles = 4 + executeDDFD(op2, IX); break; }
        case 0xFD: { uint8_t op2 = fetch8(); opcodeWord = uint16_t(0xFD00 | op2); cycles = 4 + executeDDFD(op2, IY); break; }
        default: cycles = execute(opcode); break;
    }

    recordTraceFrame(tf, pcAtStart, opcodeWord, uint8_t(cycles));
    return cycles;
}

// ── Unprefixed opcode table ──────────────────────────────────────────

int SC7852::execute(uint8_t opcode) {
    // 0x40-0x7F: LD r,r' (0x76 is HALT, the one "hole" in this block).
    if (opcode >= 0x40 && opcode <= 0x7F) {
        if (opcode == 0x76) {
            m_halted = true;
            return 4;
        }
        int dst = (opcode >> 3) & 0x07;
        int src = opcode & 0x07;
        writeReg8(dst, readReg8(src));
        return (dst == 6 || src == 6) ? 7 : 4;
    }
    // 0x80-0xBF: ALU A,r
    if (opcode >= 0x80 && opcode <= 0xBF) {
        int op = (opcode >> 3) & 0x07;
        int src = opcode & 0x07;
        uint8_t operand = readReg8(src);
        switch (op) {
            case 0: A = add8(A, operand); break;
            case 1: A = adc8(A, operand); break;
            case 2: A = sub8(A, operand); break;
            case 3: A = sbc8(A, operand); break;
            case 4: and8(operand); break;
            case 5: xor8(operand); break;
            case 6: or8(operand); break;
            default: cp8(A, operand); break;
        }
        return (src == 6) ? 7 : 4;
    }

    switch (opcode) {
        case 0x00: return 4; // NOP
        case 0x01: writeReg16_sp(0, fetch16()); return 10;
        case 0x02: bus.writeMem(uint16_t(B << 8) | C, A); return 7;
        case 0x03: writeReg16_sp(0, uint16_t(readReg16_sp(0) + 1)); return 6;
        case 0x04: B = inc8(B); return 4;
        case 0x05: B = dec8(B); return 4;
        case 0x06: B = fetch8(); return 7;
        case 0x07: { // RLCA -- unlike CB-prefixed RLC r, only C/H/N (+undoc Y/X) change; S/Z/PV untouched.
            bool carry = (A & 0x80) != 0;
            A = uint8_t((A << 1) | (carry ? 1 : 0));
            setFlagBit(kFlagH, false); setFlagBit(kFlagN, false); setFlagBit(kFlagC, carry);
            setFlagBit(kFlagY, (A & kFlagY) != 0); setFlagBit(kFlagX, (A & kFlagX) != 0);
            return 4;
        }
        case 0x08: { uint8_t ta=A,tf=F; A=A2; F=F2; A2=ta; F2=tf; return 4; } // EX AF,AF'
        case 0x09: setHL(addHL16(hl(), readReg16_sp(0))); return 11;
        case 0x0A: A = bus.readMem(uint16_t(B << 8) | C); return 7;
        case 0x0B: writeReg16_sp(0, uint16_t(readReg16_sp(0) - 1)); return 6;
        case 0x0C: C = inc8(C); return 4;
        case 0x0D: C = dec8(C); return 4;
        case 0x0E: C = fetch8(); return 7;
        case 0x0F: { // RRCA
            bool carry = (A & 0x01) != 0;
            A = uint8_t((A >> 1) | (carry ? 0x80 : 0));
            setFlagBit(kFlagH, false); setFlagBit(kFlagN, false); setFlagBit(kFlagC, carry);
            setFlagBit(kFlagY, (A & kFlagY) != 0); setFlagBit(kFlagX, (A & kFlagX) != 0);
            return 4;
        }

        case 0x10: { // DJNZ e
            int8_t e = int8_t(fetch8());
            B = uint8_t(B - 1);
            if (B != 0) { PC = uint16_t(PC + e); return 13; }
            return 8;
        }
        case 0x11: writeReg16_sp(1, fetch16()); return 10;
        case 0x12: bus.writeMem(uint16_t(D << 8) | E, A); return 7;
        case 0x13: writeReg16_sp(1, uint16_t(readReg16_sp(1) + 1)); return 6;
        case 0x14: D = inc8(D); return 4;
        case 0x15: D = dec8(D); return 4;
        case 0x16: D = fetch8(); return 7;
        case 0x17: { // RLA
            bool cin = (F & kFlagC) != 0;
            bool carry = (A & 0x80) != 0;
            A = uint8_t((A << 1) | (cin ? 1 : 0));
            setFlagBit(kFlagH, false); setFlagBit(kFlagN, false); setFlagBit(kFlagC, carry);
            setFlagBit(kFlagY, (A & kFlagY) != 0); setFlagBit(kFlagX, (A & kFlagX) != 0);
            return 4;
        }
        case 0x18: { int8_t e = int8_t(fetch8()); PC = uint16_t(PC + e); return 12; }
        case 0x19: setHL(addHL16(hl(), readReg16_sp(1))); return 11;
        case 0x1A: A = bus.readMem(uint16_t(D << 8) | E); return 7;
        case 0x1B: writeReg16_sp(1, uint16_t(readReg16_sp(1) - 1)); return 6;
        case 0x1C: E = inc8(E); return 4;
        case 0x1D: E = dec8(E); return 4;
        case 0x1E: E = fetch8(); return 7;
        case 0x1F: { // RRA
            bool cin = (F & kFlagC) != 0;
            bool carry = (A & 0x01) != 0;
            A = uint8_t((A >> 1) | (cin ? 0x80 : 0));
            setFlagBit(kFlagH, false); setFlagBit(kFlagN, false); setFlagBit(kFlagC, carry);
            setFlagBit(kFlagY, (A & kFlagY) != 0); setFlagBit(kFlagX, (A & kFlagX) != 0);
            return 4;
        }

        case 0x20: { int8_t e = int8_t(fetch8()); if (!(F & kFlagZ)) { PC = uint16_t(PC + e); return 12; } return 7; }
        case 0x21: setHL(fetch16()); return 10;
        case 0x22: { uint16_t nn = fetch16(); bus.writeMem(nn, L); bus.writeMem(uint16_t(nn+1), H); return 16; }
        case 0x23: setHL(uint16_t(hl() + 1)); return 6;
        case 0x24: H = inc8(H); return 4;
        case 0x25: H = dec8(H); return 4;
        case 0x26: H = fetch8(); return 7;
        case 0x27: daa(); return 4;
        case 0x28: { int8_t e = int8_t(fetch8()); if (F & kFlagZ) { PC = uint16_t(PC + e); return 12; } return 7; }
        case 0x29: setHL(addHL16(hl(), hl())); return 11;
        case 0x2A: { uint16_t nn = fetch16(); L = bus.readMem(nn); H = bus.readMem(uint16_t(nn+1)); return 16; }
        case 0x2B: setHL(uint16_t(hl() - 1)); return 6;
        case 0x2C: L = inc8(L); return 4;
        case 0x2D: L = dec8(L); return 4;
        case 0x2E: L = fetch8(); return 7;
        case 0x2F: A = uint8_t(~A); setFlagBit(kFlagH, true); setFlagBit(kFlagN, true);
                   setFlagBit(kFlagY, (A & kFlagY)!=0); setFlagBit(kFlagX, (A & kFlagX)!=0); return 4; // CPL

        case 0x30: { int8_t e = int8_t(fetch8()); if (!(F & kFlagC)) { PC = uint16_t(PC + e); return 12; } return 7; }
        case 0x31: SP = fetch16(); return 10;
        case 0x32: { uint16_t nn = fetch16(); bus.writeMem(nn, A); return 13; }
        case 0x33: SP = uint16_t(SP + 1); return 6;
        case 0x34: { uint8_t v = bus.readMem(hl()); bus.writeMem(hl(), inc8(v)); return 11; }
        case 0x35: { uint8_t v = bus.readMem(hl()); bus.writeMem(hl(), dec8(v)); return 11; }
        case 0x36: bus.writeMem(hl(), fetch8()); return 10;
        case 0x37: setFlagBit(kFlagH, false); setFlagBit(kFlagN, false); setFlagBit(kFlagC, true);
                   setFlagBit(kFlagY,(A&kFlagY)!=0); setFlagBit(kFlagX,(A&kFlagX)!=0); return 4; // SCF
        case 0x38: { int8_t e = int8_t(fetch8()); if (F & kFlagC) { PC = uint16_t(PC + e); return 12; } return 7; }
        case 0x39: setHL(addHL16(hl(), SP)); return 11;
        case 0x3A: { uint16_t nn = fetch16(); A = bus.readMem(nn); return 13; }
        case 0x3B: SP = uint16_t(SP - 1); return 6;
        case 0x3C: A = inc8(A); return 4;
        case 0x3D: A = dec8(A); return 4;
        case 0x3E: A = fetch8(); return 7;
        case 0x3F: { bool oldC = F & kFlagC; setFlagBit(kFlagH, oldC); setFlagBit(kFlagN, false); setFlagBit(kFlagC, !oldC);
                     setFlagBit(kFlagY,(A&kFlagY)!=0); setFlagBit(kFlagX,(A&kFlagX)!=0); return 4; } // CCF

        case 0xC0: if (condTrue(0)) { PC = popWord(); return 11; } return 5;
        case 0xC1: writeReg16_af(0, popWord()); return 10;
        case 0xC2: { uint16_t nn = fetch16(); if (condTrue(0)) PC = nn; return 10; }
        case 0xC3: PC = fetch16(); return 10;
        case 0xC4: { uint16_t nn = fetch16(); if (condTrue(0)) { pushWord(PC); PC = nn; return 17; } return 10; }
        case 0xC5: pushWord(readReg16_af(0)); return 11;
        case 0xC6: A = add8(A, fetch8()); return 7;
        case 0xC7: pushWord(PC); PC = 0x00; return 11;
        case 0xC8: if (condTrue(1)) { PC = popWord(); return 11; } return 5;
        case 0xC9: PC = popWord(); return 10;
        case 0xCA: { uint16_t nn = fetch16(); if (condTrue(1)) PC = nn; return 10; }
        case 0xCC: { uint16_t nn = fetch16(); if (condTrue(1)) { pushWord(PC); PC = nn; return 17; } return 10; }
        case 0xCD: { uint16_t nn = fetch16(); pushWord(PC); PC = nn; return 17; }
        case 0xCE: A = adc8(A, fetch8()); return 7;
        case 0xCF: pushWord(PC); PC = 0x08; return 11;

        case 0xD0: if (condTrue(2)) { PC = popWord(); return 11; } return 5;
        case 0xD1: writeReg16_af(1, popWord()); return 10;
        case 0xD2: { uint16_t nn = fetch16(); if (condTrue(2)) PC = nn; return 10; }
        case 0xD3: { uint8_t n = fetch8(); bus.writeIO(n, A); return 11; }
        case 0xD4: { uint16_t nn = fetch16(); if (condTrue(2)) { pushWord(PC); PC = nn; return 17; } return 10; }
        case 0xD5: pushWord(readReg16_af(1)); return 11;
        case 0xD6: A = sub8(A, fetch8()); return 7;
        case 0xD7: pushWord(PC); PC = 0x10; return 11;
        case 0xD8: if (condTrue(3)) { PC = popWord(); return 11; } return 5;
        case 0xD9: { uint16_t b=uint16_t(B<<8)|C, d=uint16_t(D<<8)|E, h=hl();
                     uint8_t b2=B2,c2=C2,d2=D2,e2=E2,h2=H2,l2=L2;
                     B=b2;C=c2;D=d2;E=e2;H=h2;L=l2;
                     B2=uint8_t(b>>8);C2=uint8_t(b);D2=uint8_t(d>>8);E2=uint8_t(d);H2=uint8_t(h>>8);L2=uint8_t(h);
                     return 4; } // EXX
        case 0xDA: { uint16_t nn = fetch16(); if (condTrue(3)) PC = nn; return 10; }
        case 0xDB: { uint8_t n = fetch8(); A = bus.readIO(n); return 11; }
        case 0xDC: { uint16_t nn = fetch16(); if (condTrue(3)) { pushWord(PC); PC = nn; return 17; } return 10; }
        case 0xDE: A = sbc8(A, fetch8()); return 7;
        case 0xDF: pushWord(PC); PC = 0x18; return 11;

        case 0xE0: if (condTrue(4)) { PC = popWord(); return 11; } return 5;
        case 0xE1: setHL(popWord()); return 10;
        case 0xE2: { uint16_t nn = fetch16(); if (condTrue(4)) PC = nn; return 10; }
        case 0xE3: { uint16_t tmp = bus.readMem(SP) | (uint16_t(bus.readMem(uint16_t(SP+1))) << 8);
                     bus.writeMem(SP, L); bus.writeMem(uint16_t(SP+1), H);
                     setHL(tmp); return 19; } // EX (SP),HL
        case 0xE4: { uint16_t nn = fetch16(); if (condTrue(4)) { pushWord(PC); PC = nn; return 17; } return 10; }
        case 0xE5: pushWord(hl()); return 11;
        case 0xE6: and8(fetch8()); return 7;
        case 0xE7: pushWord(PC); PC = 0x20; return 11;
        case 0xE8: if (condTrue(5)) { PC = popWord(); return 11; } return 5;
        case 0xE9: PC = hl(); return 4;
        case 0xEA: { uint16_t nn = fetch16(); if (condTrue(5)) PC = nn; return 10; }
        case 0xEB: { uint8_t th=H,tl=L; H=D; L=E; D=th; E=tl; return 4; } // EX DE,HL
        case 0xEC: { uint16_t nn = fetch16(); if (condTrue(5)) { pushWord(PC); PC = nn; return 17; } return 10; }
        case 0xEE: xor8(fetch8()); return 7;
        case 0xEF: pushWord(PC); PC = 0x28; return 11;

        case 0xF0: if (condTrue(6)) { PC = popWord(); return 11; } return 5;
        case 0xF1: writeReg16_af(3, popWord()); return 10;
        case 0xF2: { uint16_t nn = fetch16(); if (condTrue(6)) PC = nn; return 10; }
        case 0xF3: IFF1 = false; IFF2 = false; return 4;
        case 0xF4: { uint16_t nn = fetch16(); if (condTrue(6)) { pushWord(PC); PC = nn; return 17; } return 10; }
        case 0xF5: pushWord(readReg16_af(3)); return 11;
        case 0xF6: or8(fetch8()); return 7;
        case 0xF7: pushWord(PC); PC = 0x30; return 11;
        case 0xF8: if (condTrue(7)) { PC = popWord(); return 11; } return 5;
        case 0xF9: SP = hl(); return 6;
        case 0xFA: { uint16_t nn = fetch16(); if (condTrue(7)) PC = nn; return 10; }
        case 0xFB: IFF1 = true; IFF2 = true; return 4;
        case 0xFC: { uint16_t nn = fetch16(); if (condTrue(7)) { pushWord(PC); PC = nn; return 17; } return 10; }
        case 0xFE: cp8(A, fetch8()); return 7;
        case 0xFF: pushWord(PC); PC = 0x38; return 11;

        default: return 4; // unreachable given the ranges above, kept for -Wswitch safety
    }
}

// ── 0xCB prefix: rotate/shift/BIT/RES/SET ────────────────────────────

// Returned costs are the BASE (post-prefix) cost -- step() already charges
// the CB prefix's own 4 T-states on top of whatever this function returns.
int SC7852::executeCB(uint8_t opcode) {
    int group = (opcode >> 6) & 0x03;
    int mid = (opcode >> 3) & 0x07;
    int reg = opcode & 0x07;
    uint8_t v = readReg8(reg);
    int cost = (reg == 6) ? 11 : 4;

    if (group == 0) { // rotate/shift
        uint8_t result;
        switch (mid) {
            case 0: result = rlc(v); break;
            case 1: result = rrc(v); break;
            case 2: result = rl(v); break;
            case 3: result = rr(v); break;
            case 4: result = sla(v); break;
            case 5: result = sra(v); break;
            case 6: result = sll(v); break;
            default: result = srl(v); break;
        }
        writeReg8(reg, result);
        return cost;
    }
    if (group == 1) { // BIT n,r
        bool set = (v & (1 << mid)) != 0;
        setFlagBit(kFlagZ, !set);
        setFlagBit(kFlagPV, !set);
        setFlagBit(kFlagH, true);
        setFlagBit(kFlagN, false);
        setFlagBit(kFlagS, mid == 7 && set);
        // Undocumented Y/X: real hardware sources these from the internal
        // MEMPTR register for a (HL)/(IX+d)/(IY+d) operand rather than the
        // tested value itself -- MEMPTR isn't modeled by this core, so this
        // is an approximation (sourced from the operand byte instead).
        setFlagBit(kFlagY, (v & kFlagY) != 0);
        setFlagBit(kFlagX, (v & kFlagX) != 0);
        return (reg == 6) ? 8 : 4;
    }
    if (group == 2) { // RES n,r
        writeReg8(reg, uint8_t(v & ~(1 << mid)));
        return cost;
    }
    // SET n,r
    writeReg8(reg, uint8_t(v | (1 << mid)));
    return cost;
}

// ── 0xED prefix: extended instructions ────────────────────────────────

// Returned costs are the BASE (post-prefix) cost -- step() already charges
// the ED prefix's own 4 T-states on top of whatever this function returns.
int SC7852::executeED(uint8_t opcode) {
    switch (opcode) {
        // IN r,(C) / OUT (C),r
        case 0x40: B = bus.readIO(C); setSZP53(B); setFlagBit(kFlagH,false); setFlagBit(kFlagN,false); return 8;
        case 0x48: C = bus.readIO(C); setSZP53(C); setFlagBit(kFlagH,false); setFlagBit(kFlagN,false); return 8;
        case 0x50: D = bus.readIO(C); setSZP53(D); setFlagBit(kFlagH,false); setFlagBit(kFlagN,false); return 8;
        case 0x58: E = bus.readIO(C); setSZP53(E); setFlagBit(kFlagH,false); setFlagBit(kFlagN,false); return 8;
        case 0x60: H = bus.readIO(C); setSZP53(H); setFlagBit(kFlagH,false); setFlagBit(kFlagN,false); return 8;
        case 0x68: L = bus.readIO(C); setSZP53(L); setFlagBit(kFlagH,false); setFlagBit(kFlagN,false); return 8;
        case 0x70: { uint8_t t = bus.readIO(C); setSZP53(t); setFlagBit(kFlagH,false); setFlagBit(kFlagN,false); return 8; } // IN F,(C) / IN (C)
        case 0x78: A = bus.readIO(C); setSZP53(A); setFlagBit(kFlagH,false); setFlagBit(kFlagN,false); return 8;

        case 0x41: bus.writeIO(C, B); return 8;
        case 0x49: bus.writeIO(C, C); return 8;
        case 0x51: bus.writeIO(C, D); return 8;
        case 0x59: bus.writeIO(C, E); return 8;
        case 0x61: bus.writeIO(C, H); return 8;
        case 0x69: bus.writeIO(C, L); return 8;
        case 0x71: bus.writeIO(C, 0); return 8; // OUT (C),0 (undocumented)
        case 0x79: bus.writeIO(C, A); return 8;

        // SBC/ADC HL,rr
        case 0x42: setHL(sbc16(hl(), readReg16_sp(0))); return 11;
        case 0x52: setHL(sbc16(hl(), readReg16_sp(1))); return 11;
        case 0x62: setHL(sbc16(hl(), readReg16_sp(2))); return 11;
        case 0x72: setHL(sbc16(hl(), SP)); return 11;
        case 0x4A: setHL(adc16(hl(), readReg16_sp(0))); return 11;
        case 0x5A: setHL(adc16(hl(), readReg16_sp(1))); return 11;
        case 0x6A: setHL(adc16(hl(), readReg16_sp(2))); return 11;
        case 0x7A: setHL(adc16(hl(), SP)); return 11;

        // LD (nn),rr / LD rr,(nn)
        case 0x43: { uint16_t nn = fetch16(); uint16_t v = readReg16_sp(0); bus.writeMem(nn, uint8_t(v)); bus.writeMem(uint16_t(nn+1), uint8_t(v>>8)); return 16; }
        case 0x53: { uint16_t nn = fetch16(); uint16_t v = readReg16_sp(1); bus.writeMem(nn, uint8_t(v)); bus.writeMem(uint16_t(nn+1), uint8_t(v>>8)); return 16; }
        case 0x63: { uint16_t nn = fetch16(); uint16_t v = hl(); bus.writeMem(nn, uint8_t(v)); bus.writeMem(uint16_t(nn+1), uint8_t(v>>8)); return 16; }
        case 0x73: { uint16_t nn = fetch16(); bus.writeMem(nn, uint8_t(SP)); bus.writeMem(uint16_t(nn+1), uint8_t(SP>>8)); return 16; }
        case 0x4B: { uint16_t nn = fetch16(); uint8_t lo=bus.readMem(nn), hi=bus.readMem(uint16_t(nn+1)); writeReg16_sp(0, uint16_t(lo|(hi<<8))); return 16; }
        case 0x5B: { uint16_t nn = fetch16(); uint8_t lo=bus.readMem(nn), hi=bus.readMem(uint16_t(nn+1)); writeReg16_sp(1, uint16_t(lo|(hi<<8))); return 16; }
        case 0x6B: { uint16_t nn = fetch16(); uint8_t lo=bus.readMem(nn), hi=bus.readMem(uint16_t(nn+1)); setHL(uint16_t(lo|(hi<<8))); return 16; }
        case 0x7B: { uint16_t nn = fetch16(); uint8_t lo=bus.readMem(nn), hi=bus.readMem(uint16_t(nn+1)); SP = uint16_t(lo|(hi<<8)); return 16; }

        case 0x44: case 0x4C: case 0x54: case 0x5C:
        case 0x64: case 0x6C: case 0x74: case 0x7C: { // NEG
            uint8_t result = sub8(0, A);
            A = result;
            return 4;
        }
        case 0x45: case 0x55: case 0x65: case 0x75: // RETN
        case 0x4D: case 0x5D: case 0x6D: case 0x7D: // RETI
            IFF1 = IFF2;
            PC = popWord();
            return 10;

        case 0x46: case 0x4E: case 0x66: case 0x6E: IM = 0; return 4;
        case 0x56: case 0x76: IM = 1; return 4;
        case 0x5E: case 0x7E: IM = 2; return 4;

        case 0x47: I = A; return 5;
        case 0x4F: R = A; return 5;
        case 0x57: A = I; setSZ53(A); setFlagBit(kFlagH,false); setFlagBit(kFlagN,false); setFlagBit(kFlagPV, IFF2); return 5;
        case 0x5F: A = R; setSZ53(A); setFlagBit(kFlagH,false); setFlagBit(kFlagN,false); setFlagBit(kFlagPV, IFF2); return 5;

        case 0x67: { // RRD
            uint8_t mem = bus.readMem(hl());
            uint8_t newMem = uint8_t(((A & 0x0F) << 4) | (mem >> 4));
            A = uint8_t((A & 0xF0) | (mem & 0x0F));
            bus.writeMem(hl(), newMem);
            setSZP53(A); setFlagBit(kFlagH,false); setFlagBit(kFlagN,false);
            return 14;
        }
        case 0x6F: { // RLD
            uint8_t mem = bus.readMem(hl());
            uint8_t newMem = uint8_t(((mem & 0x0F) << 4) | (A & 0x0F));
            A = uint8_t((A & 0xF0) | (mem >> 4));
            bus.writeMem(hl(), newMem);
            setSZP53(A); setFlagBit(kFlagH,false); setFlagBit(kFlagN,false);
            return 14;
        }

        // Block instructions
        case 0xA0: case 0xB0: { // LDI / LDIR
            uint8_t v = bus.readMem(hl());
            bus.writeMem(uint16_t(D<<8)|E, v);
            setHL(uint16_t(hl()+1));
            uint16_t de = uint16_t((D<<8)|E); de = uint16_t(de+1); D=uint8_t(de>>8); E=uint8_t(de);
            uint16_t bcv = uint16_t((B<<8)|C); bcv = uint16_t(bcv-1); B=uint8_t(bcv>>8); C=uint8_t(bcv);
            setFlagBit(kFlagH,false); setFlagBit(kFlagN,false); setFlagBit(kFlagPV, bcv != 0);
            uint8_t nFlag = uint8_t(v + A);
            setFlagBit(kFlagY, (nFlag & 0x02) != 0); setFlagBit(kFlagX, (nFlag & 0x08) != 0);
            if (opcode == 0xB0 && bcv != 0) { PC = uint16_t(PC - 2); return 17; }
            return 12;
        }
        case 0xA8: case 0xB8: { // LDD / LDDR
            uint8_t v = bus.readMem(hl());
            bus.writeMem(uint16_t(D<<8)|E, v);
            setHL(uint16_t(hl()-1));
            uint16_t de = uint16_t((D<<8)|E); de = uint16_t(de-1); D=uint8_t(de>>8); E=uint8_t(de);
            uint16_t bcv = uint16_t((B<<8)|C); bcv = uint16_t(bcv-1); B=uint8_t(bcv>>8); C=uint8_t(bcv);
            setFlagBit(kFlagH,false); setFlagBit(kFlagN,false); setFlagBit(kFlagPV, bcv != 0);
            uint8_t nFlag = uint8_t(v + A);
            setFlagBit(kFlagY, (nFlag & 0x02) != 0); setFlagBit(kFlagX, (nFlag & 0x08) != 0);
            if (opcode == 0xB8 && bcv != 0) { PC = uint16_t(PC - 2); return 17; }
            return 12;
        }
        case 0xA1: case 0xB1: { // CPI / CPIR
            uint8_t v = bus.readMem(hl());
            uint8_t result = uint8_t(A - v);
            bool halfCarry = (A & 0x0F) < (v & 0x0F);
            setHL(uint16_t(hl()+1));
            uint16_t bcv = uint16_t((B<<8)|C); bcv = uint16_t(bcv-1); B=uint8_t(bcv>>8); C=uint8_t(bcv);
            setSZ53(result);
            setFlagBit(kFlagH, halfCarry);
            setFlagBit(kFlagPV, bcv != 0);
            setFlagBit(kFlagN, true);
            uint8_t nFlag = uint8_t(result - (halfCarry ? 1 : 0));
            setFlagBit(kFlagY, (nFlag & 0x02) != 0); setFlagBit(kFlagX, (nFlag & 0x08) != 0);
            if (opcode == 0xB1 && bcv != 0 && result != 0) { PC = uint16_t(PC - 2); return 17; }
            return 12;
        }
        case 0xA9: case 0xB9: { // CPD / CPDR
            uint8_t v = bus.readMem(hl());
            uint8_t result = uint8_t(A - v);
            bool halfCarry = (A & 0x0F) < (v & 0x0F);
            setHL(uint16_t(hl()-1));
            uint16_t bcv = uint16_t((B<<8)|C); bcv = uint16_t(bcv-1); B=uint8_t(bcv>>8); C=uint8_t(bcv);
            setSZ53(result);
            setFlagBit(kFlagH, halfCarry);
            setFlagBit(kFlagPV, bcv != 0);
            setFlagBit(kFlagN, true);
            uint8_t nFlag = uint8_t(result - (halfCarry ? 1 : 0));
            setFlagBit(kFlagY, (nFlag & 0x02) != 0); setFlagBit(kFlagX, (nFlag & 0x08) != 0);
            if (opcode == 0xB9 && bcv != 0 && result != 0) { PC = uint16_t(PC - 2); return 17; }
            return 12;
        }
        case 0xA2: case 0xB2: { // INI / INIR
            uint8_t v = bus.readIO(C);
            bus.writeMem(hl(), v);
            setHL(uint16_t(hl()+1));
            B = uint8_t(B - 1);
            setFlagBit(kFlagZ, B == 0);
            setFlagBit(kFlagN, true);
            if (opcode == 0xB2 && B != 0) { PC = uint16_t(PC - 2); return 17; }
            return 12;
        }
        case 0xAA: case 0xBA: { // IND / INDR
            uint8_t v = bus.readIO(C);
            bus.writeMem(hl(), v);
            setHL(uint16_t(hl()-1));
            B = uint8_t(B - 1);
            setFlagBit(kFlagZ, B == 0);
            setFlagBit(kFlagN, true);
            if (opcode == 0xBA && B != 0) { PC = uint16_t(PC - 2); return 17; }
            return 12;
        }
        case 0xA3: case 0xB3: { // OUTI / OTIR
            uint8_t v = bus.readMem(hl());
            B = uint8_t(B - 1);
            bus.writeIO(C, v);
            setHL(uint16_t(hl()+1));
            setFlagBit(kFlagZ, B == 0);
            setFlagBit(kFlagN, true);
            if (opcode == 0xB3 && B != 0) { PC = uint16_t(PC - 2); return 17; }
            return 12;
        }
        case 0xAB: case 0xBB: { // OUTD / OTDR
            uint8_t v = bus.readMem(hl());
            B = uint8_t(B - 1);
            bus.writeIO(C, v);
            setHL(uint16_t(hl()-1));
            setFlagBit(kFlagZ, B == 0);
            setFlagBit(kFlagN, true);
            if (opcode == 0xBB && B != 0) { PC = uint16_t(PC - 2); return 17; }
            return 12;
        }

        default: return 4; // undocumented ED opcode: treated as a NOP (base cost)
    }
}

// ── 0xDD/0xFD prefix: IX/IY forms ──────────────────────────────────────
//
// Real Z-80 behavior: DD/FD only redirects an opcode's H/L/(HL) references
// to IXH/IXL/(IX+d) (or the IY equivalents); every other opcode runs
// exactly as its unprefixed form, with the prefix wasted (documented
// undocumented behavior). Within the LD r,r'/ALU-A,r blocks specifically:
// when the instruction's *memory* operand slot (register code 6) is used
// -- i.e. it's genuinely a (IX+d) access -- any OTHER register slot in
// that same instruction (e.g. "LD (IX+d),H") stays the PLAIN H/L
// register, not IXH/IXL; the IXH/IXL substitution only applies when
// neither operand is the memory slot.

// Returned costs are the BASE (unprefixed-equivalent) cost only -- step()
// already charges the DD/FD prefix's own 4 T-states on top of whatever
// this function returns, so every case here returns (documented total - 4).

int SC7852::executeDDFD(uint8_t opcode, uint16_t& ixy) {
    switch (opcode) {
        case 0x09: ixy = addHL16(ixy, readReg16_sp(0)); return 11;
        case 0x19: ixy = addHL16(ixy, readReg16_sp(1)); return 11;
        case 0x29: ixy = addHL16(ixy, ixy); return 11;
        case 0x39: ixy = addHL16(ixy, SP); return 11;
        case 0x21: ixy = fetch16(); return 10;
        case 0x22: { uint16_t nn=fetch16(); bus.writeMem(nn, uint8_t(ixy)); bus.writeMem(uint16_t(nn+1), uint8_t(ixy>>8)); return 16; }
        case 0x2A: { uint16_t nn=fetch16(); uint8_t lo=bus.readMem(nn), hi=bus.readMem(uint16_t(nn+1)); ixy = uint16_t(lo|(hi<<8)); return 16; }
        case 0x23: ixy = uint16_t(ixy+1); return 6;
        case 0x2B: ixy = uint16_t(ixy-1); return 6;
        case 0xE1: ixy = popWord(); return 10;
        case 0xE5: pushWord(ixy); return 11;
        case 0xE3: { uint16_t tmp = uint16_t(bus.readMem(SP) | (uint16_t(bus.readMem(uint16_t(SP+1)))<<8));
                     bus.writeMem(SP, uint8_t(ixy)); bus.writeMem(uint16_t(SP+1), uint8_t(ixy>>8));
                     ixy = tmp; return 19; }
        case 0xE9: PC = ixy; return 4;
        case 0xF9: SP = ixy; return 6;
        case 0x24: { uint8_t h=inc8(uint8_t(ixy>>8)); ixy=uint16_t((ixy&0x00FF)|(uint16_t(h)<<8)); return 4; }
        case 0x25: { uint8_t h=dec8(uint8_t(ixy>>8)); ixy=uint16_t((ixy&0x00FF)|(uint16_t(h)<<8)); return 4; }
        case 0x26: { uint8_t n=fetch8(); ixy=uint16_t((ixy&0x00FF)|(uint16_t(n)<<8)); return 7; }
        case 0x2C: { uint8_t l=inc8(uint8_t(ixy)); ixy=uint16_t((ixy&0xFF00)|l); return 4; }
        case 0x2D: { uint8_t l=dec8(uint8_t(ixy)); ixy=uint16_t((ixy&0xFF00)|l); return 4; }
        case 0x2E: { uint8_t n=fetch8(); ixy=uint16_t((ixy&0xFF00)|n); return 7; }
        case 0x34: { int8_t d=int8_t(fetch8()); uint16_t addr=uint16_t(ixy+d); uint8_t v=bus.readMem(addr); bus.writeMem(addr, inc8(v)); return 19; }
        case 0x35: { int8_t d=int8_t(fetch8()); uint16_t addr=uint16_t(ixy+d); uint8_t v=bus.readMem(addr); bus.writeMem(addr, dec8(v)); return 19; }
        case 0x36: { int8_t d=int8_t(fetch8()); uint8_t n=fetch8(); bus.writeMem(uint16_t(ixy+d), n); return 15; }
        case 0xCB: { int8_t d=int8_t(fetch8()); uint8_t sub=fetch8(); return executeDDFDCB(sub, ixy, d); }
        default: break;
    }

    if (opcode >= 0x40 && opcode <= 0x7F) {
        if (opcode == 0x76) { m_halted = true; return 4; } // DD/FD 76: still plain HALT
        int dst = (opcode >> 3) & 0x07;
        int src = opcode & 0x07;
        if (dst == 6) {
            int8_t d = int8_t(fetch8());
            bus.writeMem(uint16_t(ixy + d), readReg8(src)); // src stays plain H/L if 4/5
            return 15;
        }
        if (src == 6) {
            int8_t d = int8_t(fetch8());
            writeReg8(dst, bus.readMem(uint16_t(ixy + d))); // dst stays plain H/L if 4/5
            return 15;
        }
        auto readSub = [&](int code) -> uint8_t {
            if (code == 4) return uint8_t(ixy >> 8);
            if (code == 5) return uint8_t(ixy);
            return readReg8(code);
        };
        auto writeSub = [&](int code, uint8_t v) {
            if (code == 4) { ixy = uint16_t((ixy & 0x00FF) | (uint16_t(v) << 8)); return; }
            if (code == 5) { ixy = uint16_t((ixy & 0xFF00) | v); return; }
            writeReg8(code, v);
        };
        writeSub(dst, readSub(src));
        return 4;
    }

    if (opcode >= 0x80 && opcode <= 0xBF) {
        int op = (opcode >> 3) & 0x07;
        int src = opcode & 0x07;
        uint8_t operand;
        int cost;
        if (src == 6) {
            int8_t d = int8_t(fetch8());
            operand = bus.readMem(uint16_t(ixy + d));
            cost = 15;
        } else if (src == 4) {
            operand = uint8_t(ixy >> 8);
            cost = 4;
        } else if (src == 5) {
            operand = uint8_t(ixy);
            cost = 4;
        } else {
            operand = readReg8(src);
            cost = 4;
        }
        switch (op) {
            case 0: A = add8(A, operand); break;
            case 1: A = adc8(A, operand); break;
            case 2: A = sub8(A, operand); break;
            case 3: A = sbc8(A, operand); break;
            case 4: and8(operand); break;
            case 5: xor8(operand); break;
            case 6: or8(operand); break;
            default: cp8(A, operand); break;
        }
        return cost;
    }

    // Every other opcode: DD/FD had no effect on it -- run it plain. (The
    // prefix's own wasted M1 cycle was already charged by step()'s +4.)
    return execute(opcode);
}

// ── 0xDD/0xFD 0xCB dd oo: IX/IY bit operations ─────────────────────────
//
// Documented behavior only: the result is written to (ixy+d). Real
// hardware also copies rotate/shift/RES/SET results into the register the
// opcode's low 3 bits name (when not 6) as an undocumented side effect --
// not modeled here (no known PC-1600 ROM code path is expected to depend
// on it; flagged here rather than silently guessed at, consistent with
// this project's stance on unconfirmed hardware behavior elsewhere).
int SC7852::executeDDFDCB(uint8_t opcode, uint16_t ixy, int8_t d) {
    uint16_t addr = uint16_t(ixy + d);
    int group = (opcode >> 6) & 0x03;
    int mid = (opcode >> 3) & 0x07;
    uint8_t v = bus.readMem(addr);

    if (group == 1) { // BIT n,(ixy+d)
        bool set = (v & (1 << mid)) != 0;
        setFlagBit(kFlagZ, !set);
        setFlagBit(kFlagPV, !set);
        setFlagBit(kFlagH, true);
        setFlagBit(kFlagN, false);
        setFlagBit(kFlagS, mid == 7 && set);
        setFlagBit(kFlagY, (uint8_t(addr >> 8) & kFlagY) != 0); // MEMPTR approximation, see .hpp
        setFlagBit(kFlagX, (uint8_t(addr >> 8) & kFlagX) != 0);
        return 16; // base cost; step() already charged the DD/FD prefix's 4 T-states
    }
    uint8_t result;
    if (group == 0) {
        switch (mid) {
            case 0: result = rlc(v); break;
            case 1: result = rrc(v); break;
            case 2: result = rl(v); break;
            case 3: result = rr(v); break;
            case 4: result = sla(v); break;
            case 5: result = sra(v); break;
            case 6: result = sll(v); break;
            default: result = srl(v); break;
        }
    } else if (group == 2) {
        result = uint8_t(v & ~(1 << mid));
    } else {
        result = uint8_t(v | (1 << mid));
    }
    bus.writeMem(addr, result);
    return 19; // base cost; step() already charged the DD/FD prefix's 4 T-states
}

// ── Trace / debug (mirrors LH5801's implementation exactly) ────────────

void SC7852::recordTraceFrame(uint32_t tf, uint16_t pcAtStart, uint16_t opcodeWord, uint8_t cycles) {
    if ((tf & (TRACE_PC | TRACE_REGS_LIGHT | TRACE_REGS_FULL)) == 0) return;

    Z80CpuFrame f{};
    f.seqno = m_traceSeqno++;
    f.pc = pcAtStart;
    f.opcode = opcodeWord;
    f.cycles = cycles;
    if (tf & (TRACE_REGS_LIGHT | TRACE_REGS_FULL)) {
        f.af = af(); f.bc = bc(); f.de = de(); f.hl = hl(); f.ix = IX; f.iy = IY; f.sp = SP;
    }
    if (tf & TRACE_REGS_FULL) {
        f.i = I; f.r = R; f.iff1 = IFF1; f.iff2 = IFF2; f.im = IM;
    }

    std::lock_guard<std::mutex> lock(m_traceMutex);
    m_ring[m_totalWritten & kRingMask] = f;
    m_totalWritten++;
}

uint32_t SC7852::drainTraceEvents(Z80CpuFrame* out, uint32_t max, uint32_t* outLost) {
    std::lock_guard<std::mutex> lock(m_traceMutex);
    uint32_t available = m_totalWritten - m_drainCursor;
    uint32_t lost = 0;
    if (available > kRingSize) {
        lost = available - kRingSize;
        m_drainCursor = m_totalWritten - kRingSize;
        available = kRingSize;
    }
    if (outLost) *outLost = lost;
    uint32_t n = std::min(max, available);
    for (uint32_t i = 0; i < n; i++) {
        out[i] = m_ring[(m_drainCursor + i) & kRingMask];
    }
    m_drainCursor += n;
    return n;
}

uint32_t SC7852::peekTraceEvents(Z80CpuFrame* out, uint32_t max) {
    std::lock_guard<std::mutex> lock(m_traceMutex);
    uint32_t available = std::min(m_totalWritten, kRingSize);
    uint32_t n = std::min(max, available);
    uint32_t start = m_totalWritten - n;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = m_ring[(start + i) & kRingMask];
    }
    return n;
}

void SC7852::addBreakpoint(uint16_t addr) {
    std::lock_guard<std::mutex> lock(m_traceMutex);
    if (!std::binary_search(m_breakpoints.begin(), m_breakpoints.end(), addr)) {
        m_breakpoints.insert(std::upper_bound(m_breakpoints.begin(), m_breakpoints.end(), addr), addr);
    }
}
void SC7852::removeBreakpoint(uint16_t addr) {
    std::lock_guard<std::mutex> lock(m_traceMutex);
    auto it = std::lower_bound(m_breakpoints.begin(), m_breakpoints.end(), addr);
    if (it != m_breakpoints.end() && *it == addr) m_breakpoints.erase(it);
}
void SC7852::clearBreakpoints() {
    std::lock_guard<std::mutex> lock(m_traceMutex);
    m_breakpoints.clear();
}
bool SC7852::consumeBreakpointHit() {
    bool hit = m_breakpointHit;
    m_breakpointHit = false;
    return hit;
}
