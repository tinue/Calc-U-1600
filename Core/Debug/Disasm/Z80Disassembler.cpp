#include "Z80Disassembler.hpp"

#include <string>

namespace disasm {

namespace {

const char* const kCC[8] = {"nz", "z", "nc", "c", "po", "pe", "p", "m"};
const char* const kALU[8] = {"add a,", "adc a,", "sub ", "sbc a,", "and ", "xor ", "or ", "cp "};
const char* const kROT[8] = {"rlc", "rrc", "rl", "rr", "sla", "sra", "sll", "srl"};
const char* const kROTA[8] = {"rlca", "rrca", "rla", "rra", "daa", "cpl", "scf", "ccf"};
const char* const kIM[8] = {"0", "0", "1", "2", "0", "0", "1", "2"};
const char* const kBLOCK[4][4] = {
    {"ldi", "cpi", "ini", "outi"},
    {"ldd", "cpd", "ind", "outd"},
    {"ldir", "cpir", "inir", "otir"},
    {"lddr", "cpdr", "indr", "otdr"},
};

// Decoding state for one instruction: where the next byte comes from and
// which index register (if any) replaces HL.
struct Ctx {
    uint16_t pc;
    const FetchFn& fetch;
    const SymbolFn& symbols;
    char index = 0;          // 0, 'x' (DD) or 'y' (FD)
    bool haveDisp = false;   // DD/FD CB: the displacement precedes the opcode
    int8_t disp = 0;
    Decoded& d;

    uint8_t byte() { return fetch(pc++); }
    uint16_t word() { uint8_t lo = byte(); uint8_t hi = byte(); return uint16_t(lo | (hi << 8)); }

    std::string hl() const { return index ? std::string("i") + index : "hl"; }
    std::string indexed() {
        if (!haveDisp) { disp = int8_t(byte()); haveDisp = true; }
        std::string s = "(i" + std::string(1, index);
        if (disp < 0) s += "-" + hex8(uint8_t(-disp));
        else          s += "+" + hex8(uint8_t(disp));
        return s + ")";
    }
    /// r[code]; `plainHL` keeps H/L unreplaced (the other operand is (ix+d)).
    std::string reg(int code, bool plainHL = false) {
        static const char* const kR[8] = {"b", "c", "d", "e", "h", "l", "(hl)", "a"};
        if (index) {
            if (code == 6) return indexed();
            if (!plainHL && (code == 4 || code == 5)) return std::string("i") + index + (code == 4 ? "h" : "l");
        }
        return kR[code];
    }
    std::string rp(int p) const { static const char* const kRP[4] = {"bc", "de", "hl", "sp"}; return p == 2 ? hl() : kRP[p]; }
    std::string rp2(int p) const { static const char* const kRP2[4] = {"bc", "de", "hl", "af"}; return p == 2 ? hl() : kRP2[p]; }
    std::string mem16() { return "(" + addrText(word(), symbols) + ")"; }
    std::string target(Flow flow, uint16_t t) {
        d.flow = flow;
        d.hasTarget = true;
        d.target = t;
        return addrText(t, symbols);
    }
    std::string relative(Flow flow) {
        const int8_t e = int8_t(byte());
        return target(flow, uint16_t(pc + e));
    }
};

std::string decodeCB(Ctx& c) {
    const uint8_t op = c.byte();
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    if (c.index) {
        const std::string m = c.indexed(); // displacement already read, before op
        static const char* const kR[8] = {"b", "c", "d", "e", "h", "l", "", "a"};
        const std::string copy = z == 6 ? "" : std::string(",") + kR[z]; // undocumented: result also lands in r
        switch (x) {
            case 0: return std::string(kROT[y]) + " " + m + copy;
            case 1: return "bit " + std::to_string(y) + "," + m;
            case 2: return "res " + std::to_string(y) + "," + m + copy;
            default: return "set " + std::to_string(y) + "," + m + copy;
        }
    }
    const std::string r = c.reg(z);
    switch (x) {
        case 0: return std::string(kROT[y]) + " " + r;
        case 1: return "bit " + std::to_string(y) + "," + r;
        case 2: return "res " + std::to_string(y) + "," + r;
        default: return "set " + std::to_string(y) + "," + r;
    }
}

std::string decodeED(Ctx& c, uint8_t op) {
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    static const char* const kR[8] = {"b", "c", "d", "e", "h", "l", "", "a"};
    if (x == 1) {
        switch (z) {
            case 0: return y == 6 ? "in f,(c)" : std::string("in ") + kR[y] + ",(c)";
            case 1: return y == 6 ? "out (c),0" : std::string("out (c),") + kR[y];
            case 2: { static const char* const kRP[4] = {"bc", "de", "hl", "sp"}; return std::string(q ? "adc hl," : "sbc hl,") + kRP[p]; }
            case 3: {
                static const char* const kRP[4] = {"bc", "de", "hl", "sp"};
                if (q == 0) { std::string m = c.mem16(); return "ld " + m + "," + kRP[p]; }
                return std::string("ld ") + kRP[p] + "," + c.mem16();
            }
            case 4: return "neg";
            case 5: c.d.flow = Flow::Return; return y == 1 ? "reti" : "retn";
            case 6: return std::string("im ") + kIM[y];
            default: {
                static const char* const kMisc[8] = {"ld i,a", "ld r,a", "ld a,i", "ld a,r", "rrd", "rld", nullptr, nullptr};
                if (kMisc[y]) return kMisc[y];
                break;
            }
        }
    } else if (x == 2 && z <= 3 && y >= 4) {
        return kBLOCK[y - 4][z];
    }
    c.d.illegal = true;
    return ".db " + hex8(0xED) + "," + hex8(op);
}

std::string decodeMain(Ctx& c, uint8_t op) {
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    switch (x) {
        case 0:
            switch (z) {
                case 0:
                    switch (y) {
                        case 0: return "nop";
                        case 1: return "ex af,af'";
                        case 2: return "djnz " + c.relative(Flow::CondJump);
                        case 3: return "jr " + c.relative(Flow::Jump);
                        default: return std::string("jr ") + kCC[y - 4] + "," + c.relative(Flow::CondJump);
                    }
                case 1:
                    if (q == 0) return "ld " + c.rp(p) + "," + hex16(c.word());
                    return "add " + c.hl() + "," + c.rp(p);
                case 2:
                    switch (y) {
                        case 0: return "ld (bc),a";
                        case 1: return "ld a,(bc)";
                        case 2: return "ld (de),a";
                        case 3: return "ld a,(de)";
                        case 4: { std::string m = c.mem16(); return "ld " + m + "," + c.hl(); }
                        case 5: return "ld " + c.hl() + "," + c.mem16();
                        case 6: { std::string m = c.mem16(); return "ld " + m + ",a"; }
                        default: return "ld a," + c.mem16();
                    }
                case 3: return (q ? "dec " : "inc ") + c.rp(p);
                case 4: return "inc " + c.reg(y);
                case 5: return "dec " + c.reg(y);
                case 6: { std::string r = c.reg(y); return "ld " + r + "," + hex8(c.byte()); } // (ix+d) before n
                default: return kROTA[y];
            }
        case 1:
            if (y == 6 && z == 6) { c.d.flow = Flow::Halt; return "halt"; }
            {
                const bool mem = y == 6 || z == 6; // ld h,(ix+d) keeps the real H
                std::string dst = c.reg(y, mem);
                std::string src = c.reg(z, mem);
                return "ld " + dst + "," + src;
            }
        case 2:
            return kALU[y] + c.reg(z);
        default:
            switch (z) {
                case 0: c.d.flow = Flow::CondReturn; return std::string("ret ") + kCC[y];
                case 1:
                    if (q == 0) return "pop " + c.rp2(p);
                    switch (p) {
                        case 0: c.d.flow = Flow::Return; return "ret";
                        case 1: return "exx";
                        case 2: c.d.flow = Flow::Jump; return "jp (" + c.hl() + ")"; // indirect: no static target
                        default: return "ld sp," + c.hl();
                    }
                case 2: return std::string("jp ") + kCC[y] + "," + c.target(Flow::CondJump, c.word());
                case 3:
                    switch (y) {
                        case 0: return "jp " + c.target(Flow::Jump, c.word());
                        case 2: return "out (" + hex8(c.byte()) + "),a";
                        case 3: return "in a,(" + hex8(c.byte()) + ")";
                        case 4: return "ex (sp)," + c.hl();
                        case 5: return "ex de,hl"; // never affected by DD/FD
                        case 6: return "di";
                        case 7: return "ei";
                        default: return {}; // CB: handled by the caller
                    }
                case 4: return std::string("call ") + kCC[y] + "," + c.target(Flow::CondCall, c.word());
                case 5:
                    if (q == 0) return "push " + c.rp2(p);
                    return "call " + c.target(Flow::Call, c.word()); // p != 0 are prefixes, handled by the caller
                case 6: return kALU[y] + hex8(c.byte());
                default: return "rst " + c.target(Flow::Call, uint16_t(y * 8));
            }
    }
}

} // namespace

Decoded decodeZ80(uint16_t addr, const FetchFn& fetch, const SymbolFn& symbols) {
    Decoded d;
    Ctx c{addr, fetch, symbols, 0, false, 0, d};
    uint8_t op = c.byte();
    if (op == 0xDD || op == 0xFD) {
        const uint8_t next = fetch(c.pc);
        if (next == 0xDD || next == 0xFD) { // the CPU drops this prefix; the next one decides
            d.text = ".db " + hex8(op);
            d.len = 1;
            d.illegal = true;
            return d;
        }
        if (next == 0xED) { // DD/FD before ED is ignored
            op = c.byte();
        } else {
            c.index = op == 0xDD ? 'x' : 'y';
            op = c.byte();
        }
    }
    if (op == 0xED) {
        d.text = decodeED(c, c.byte());
    } else if (op == 0xCB) {
        if (c.index) { c.disp = int8_t(c.byte()); c.haveDisp = true; } // DD CB d op
        d.text = decodeCB(c);
    } else {
        d.text = decodeMain(c, op);
    }
    d.len = uint8_t(c.pc - addr);
    return d;
}

} // namespace disasm
