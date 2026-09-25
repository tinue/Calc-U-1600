#include "CpuRegisters.hpp"

#include <algorithm>

#include "../CPU/LH5801/LH5801.hpp"
#include "../CPU/SC7852/SC7852.hpp"

namespace debug {

namespace {

// Flag lookup shared by both families: "<flag>f" names a bit of the status
// register (see flagNames()).
int flagBit(CpuKind kind, const std::string& name) {
    if (name.size() < 2 || name.back() != 'f') return -1;
    const std::string flag = name.substr(0, name.size() - 1);
    const auto& names = flagNames(kind);
    for (size_t i = 0; i < names.size(); i++)
        if (!names[i].empty() && names[i] == flag) return int(i);
    return -1;
}

// The LH580x register set, from plain values -- used for the live CPU and
// for history frames alike.
struct LhValues {
    uint8_t a;
    uint16_t x, y, u, s, p;
    uint8_t t;
    bool pu, pv;
};

std::vector<Register> lhList(const LhValues& v) {
    return {
        {"a", v.a, 8},  {"x", v.x, 16}, {"y", v.y, 16}, {"u", v.u, 16},
        {"s", v.s, 16}, {"p", v.p, 16}, {"t", v.t, 8},
        {"pu", v.pu ? 1u : 0u, 1}, {"pv", v.pv ? 1u : 0u, 1},
    };
}

struct ZValues {
    uint16_t af, bc, de, hl, ix, iy, sp, pc, af2, bc2, de2, hl2;
    uint8_t i, r, im;
    bool iff1, iff2;
};

std::vector<Register> zList(const ZValues& v) {
    return {
        {"af", v.af, 16}, {"bc", v.bc, 16}, {"de", v.de, 16}, {"hl", v.hl, 16},
        {"ix", v.ix, 16}, {"iy", v.iy, 16}, {"sp", v.sp, 16}, {"pc", v.pc, 16},
        {"af'", v.af2, 16}, {"bc'", v.bc2, 16}, {"de'", v.de2, 16}, {"hl'", v.hl2, 16},
        {"i", v.i, 8}, {"r", v.r, 8}, {"im", v.im, 8},
        {"iff1", v.iff1 ? 1u : 0u, 1}, {"iff2", v.iff2 ? 1u : 0u, 1},
    };
}

std::string canonicalZ80(std::string name) {
    // "af2" is the expression-friendly spelling of "af'"
    if (name.size() == 3 && name[2] == '2' && (name == "af2" || name == "bc2" || name == "de2" || name == "hl2"))
        name[2] = '\'';
    return name;
}

} // namespace

// ── LH580x ────────────────────────────────────────────────────────────────

std::vector<Register> lhRegisters(const LH5801& cpu) {
    std::vector<Register> list = lhList({cpu.a(), cpu.x(), cpu.y(), cpu.u(), cpu.sp(), cpu.pc(),
                                         cpu.statusReg(), cpu.pu(), cpu.pv()});
    list.push_back({"tm", cpu.timer(), 16});
    return list;
}

bool lhReadRegister(const LH5801& cpu, const std::string& name, uint32_t* value) {
    for (const Register& r : lhRegisters(cpu))
        if (r.name == name) { *value = r.value; return true; }
    static const char* const kHalves[6] = {"xl", "xh", "yl", "yh", "ul", "uh"};
    const uint8_t halves[6] = {cpu.xl(), cpu.xh(), cpu.yl(), cpu.yh(), cpu.ul(), cpu.uh()};
    for (int i = 0; i < 6; i++)
        if (name == kHalves[i]) { *value = halves[i]; return true; }
    const int bit = flagBit(CpuKind::LH5801, name);
    if (bit >= 0) { *value = (cpu.statusReg() >> bit) & 1u; return true; }
    return false;
}

bool lhWriteRegister(LH5801& cpu, const std::string& name, uint32_t v) {
    const uint8_t b = uint8_t(v);
    const uint16_t w = uint16_t(v);
    if (name == "a") cpu.setA(b);
    else if (name == "x") cpu.setX(w);
    else if (name == "y") cpu.setY(w);
    else if (name == "u") cpu.setU(w);
    else if (name == "s") cpu.setSP(w);
    else if (name == "p") cpu.setPC(w);
    else if (name == "t") cpu.setStatusReg(b);
    else if (name == "pu") cpu.setPU(v != 0);
    else if (name == "pv") cpu.setPV(v != 0);
    else if (name == "tm") cpu.setTimer(uint16_t(v & 0x1FF));
    else if (name == "xl") cpu.setXL(b);
    else if (name == "xh") cpu.setXH(b);
    else if (name == "yl") cpu.setYL(b);
    else if (name == "yh") cpu.setYH(b);
    else if (name == "ul") cpu.setUL(b);
    else if (name == "uh") cpu.setUH(b);
    else {
        const int bit = flagBit(CpuKind::LH5801, name);
        if (bit < 0) return false;
        const uint8_t mask = uint8_t(1u << bit);
        cpu.setStatusReg(v ? uint8_t(cpu.statusReg() | mask) : uint8_t(cpu.statusReg() & ~mask));
    }
    return true;
}

HistoryEntry lhHistoryEntry(const LH5801HistoryFrame& f) {
    HistoryEntry e;
    e.pc = f.pc;
    e.len = f.len;
    std::copy(f.bytes, f.bytes + sizeof f.bytes, e.bytes);
    e.interrupt = f.interrupt;
    e.registers = lhList({f.a, f.x, f.y, f.u, f.s, f.p, f.t, f.pu, f.pv});
    return e;
}

// ── Z-80 ──────────────────────────────────────────────────────────────────

std::vector<Register> z80Registers(const SC7852& cpu) {
    return zList({cpu.af(), cpu.bc(), cpu.de(), cpu.hl(), cpu.ix(), cpu.iy(), cpu.sp(), cpu.pc(),
                  cpu.af2(), cpu.bc2(), cpu.de2(), cpu.hl2(), cpu.i(), cpu.r(), cpu.im(), cpu.iff1(), cpu.iff2()});
}

bool z80ReadRegister(const SC7852& cpu, const std::string& raw, uint32_t* value) {
    const std::string name = canonicalZ80(raw);
    for (const Register& r : z80Registers(cpu))
        if (r.name == name) { *value = r.value; return true; }
    struct Half { const char* name; uint16_t pair; bool high; };
    const Half halves[] = {
        {"a", cpu.af(), true}, {"f", cpu.af(), false}, {"b", cpu.bc(), true}, {"c", cpu.bc(), false},
        {"d", cpu.de(), true}, {"e", cpu.de(), false}, {"h", cpu.hl(), true}, {"l", cpu.hl(), false},
        {"ixh", cpu.ix(), true}, {"ixl", cpu.ix(), false}, {"iyh", cpu.iy(), true}, {"iyl", cpu.iy(), false},
    };
    for (const Half& h : halves)
        if (name == h.name) { *value = h.high ? uint32_t(h.pair >> 8) : uint32_t(h.pair & 0xFF); return true; }
    const int bit = flagBit(CpuKind::Z80, name);
    if (bit >= 0) { *value = (cpu.f() >> bit) & 1u; return true; }
    return false;
}

bool z80WriteRegister(SC7852& cpu, const std::string& raw, uint32_t v) {
    const std::string name = canonicalZ80(raw);
    const uint16_t w = uint16_t(v);
    const uint8_t b = uint8_t(v);
    auto hi = [b](uint16_t pair) { return uint16_t((b << 8) | (pair & 0xFF)); };
    auto lo = [b](uint16_t pair) { return uint16_t((pair & 0xFF00) | b); };
    if (name == "af") cpu.setAF(w);
    else if (name == "bc") cpu.setBC(w);
    else if (name == "de") cpu.setDE(w);
    else if (name == "hl") cpu.setHL(w);
    else if (name == "ix") cpu.setIX(w);
    else if (name == "iy") cpu.setIY(w);
    else if (name == "sp") cpu.setSP(w);
    else if (name == "pc") cpu.setPC(w);
    else if (name == "af'") cpu.setAF2(w);
    else if (name == "bc'") cpu.setBC2(w);
    else if (name == "de'") cpu.setDE2(w);
    else if (name == "hl'") cpu.setHL2(w);
    else if (name == "i") cpu.setI(b);
    else if (name == "r") cpu.setR(b);
    else if (name == "im") cpu.setIM(b);
    else if (name == "iff1") cpu.setIFF1(v != 0);
    else if (name == "iff2") cpu.setIFF2(v != 0);
    else if (name == "a") cpu.setAF(hi(cpu.af()));
    else if (name == "f") cpu.setAF(lo(cpu.af()));
    else if (name == "b") cpu.setBC(hi(cpu.bc()));
    else if (name == "c") cpu.setBC(lo(cpu.bc()));
    else if (name == "d") cpu.setDE(hi(cpu.de()));
    else if (name == "e") cpu.setDE(lo(cpu.de()));
    else if (name == "h") cpu.setHL(hi(cpu.hl()));
    else if (name == "l") cpu.setHL(lo(cpu.hl()));
    else if (name == "ixh") cpu.setIX(hi(cpu.ix()));
    else if (name == "ixl") cpu.setIX(lo(cpu.ix()));
    else if (name == "iyh") cpu.setIY(hi(cpu.iy()));
    else if (name == "iyl") cpu.setIY(lo(cpu.iy()));
    else {
        const int bit = flagBit(CpuKind::Z80, name);
        if (bit < 0) return false;
        const uint8_t mask = uint8_t(1u << bit);
        const uint8_t f = v ? uint8_t(cpu.f() | mask) : uint8_t(cpu.f() & ~mask);
        cpu.setAF(uint16_t((cpu.af() & 0xFF00) | f));
    }
    return true;
}

HistoryEntry z80HistoryEntry(const Z80HistoryFrame& f) {
    HistoryEntry e;
    e.pc = f.pc;
    e.len = f.len;
    std::copy(f.bytes, f.bytes + sizeof f.bytes, e.bytes);
    e.interrupt = f.interrupt;
    e.registers = zList({f.af, f.bc, f.de, f.hl, f.ix, f.iy, f.sp, f.pcAfter, f.af2, f.bc2, f.de2, f.hl2,
                         f.i, f.r, f.im, f.iff1, f.iff2});
    return e;
}

} // namespace debug
