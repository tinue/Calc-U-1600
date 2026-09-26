// Headless tests for the debugger's disassemblers
// (Core/Debug/Disasm/) and the CPUs' always-on history rings
// (Core/CPU/HistoryRing.hpp). Same assert-and-tally style as
// lh5801_tests.cpp.
//
// The LH5801 text vectors are the instruction/byte pairs of the
// sdaslh5801 assembler's own test table (sdcc-pc1500
// sdas/aslh5801/test_lh5801.py), so a disassembly reads back as source the
// project's assembler accepts. `ldx x` / `stx x` (FD 08 / FD 4A) are left
// out: the assembler encodes them, but the CPU core treats them as
// undocumented, and the disassembler follows the core.
//
// The opcode-map sweeps check every decode against the CPU cores
// themselves: for every opcode that falls through, the decoded length must
// equal how far step() moved the program counter, and "illegal" must agree
// with the core's own undocumented-opcode detection.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../CPU/LH5801/LH5801.hpp"
#include "../CPU/SC7852/SC7852.hpp"
#include "../Debug/Disasm/LH5801Disassembler.hpp"
#include "../Debug/Disasm/Z80Disassembler.hpp"
#include "TestRoms.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_TEXT(got, want) do { \
    if ((got) == (want)) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, \
                                  std::string(got).c_str(), std::string(want).c_str()); } \
} while (0)

class LhBus : public LH5801Bus {
public:
    uint8_t mem[65536]{};
    uint8_t me1[65536]{};
    uint8_t readME0(uint16_t a) override { return mem[a]; }
    void    writeME0(uint16_t a, uint8_t v) override { mem[a] = v; }
    uint8_t readME1(uint16_t a) override { return me1[a]; }
    void    writeME1(uint16_t a, uint8_t v) override { me1[a] = v; }
};

class ZBus : public SC7852Bus {
public:
    uint8_t mem[65536]{};
    uint8_t readMem(uint16_t a) override { return mem[a]; }
    void    writeMem(uint16_t a, uint8_t v) override { mem[a] = v; }
    bool intLine = false;
    bool interruptLevel() const override { return intLine; }
};

disasm::FetchFn fetchFrom(const std::vector<uint8_t>& bytes, uint16_t base) {
    return [&bytes, base](uint16_t a) -> uint8_t {
        uint16_t i = uint16_t(a - base);
        return i < bytes.size() ? bytes[i] : 0x00;
    };
}

// ── LH5801 ────────────────────────────────────────────────────────────────

struct LhVector { const char* text; std::vector<uint8_t> bytes; int len; };

void test_lh5801_assembler_vectors() {
    static const LhVector kVectors[] = {
    {"adc xl", {0x02}, 1},
    {"adc yl", {0x12}, 1},
    {"adc ul", {0x22}, 1},
    {"adc xh", {0x82}, 1},
    {"adc yh", {0x92}, 1},
    {"adc uh", {0xA2}, 1},
    {"adc (x)", {0x03}, 1},
    {"adc (y)", {0x13}, 1},
    {"adc (u)", {0x23}, 1},
    {"adc (0x1234)", {0xA3, 0x12, 0x34}, 3},
    {"adc #(x)", {0xFD, 0x03}, 2},
    {"adc #(y)", {0xFD, 0x13}, 2},
    {"adc #(u)", {0xFD, 0x23}, 2},
    {"adc #(0x1234)", {0xFD, 0xA3, 0x12, 0x34}, 4},
    {"sbc xl", {0x00}, 1},
    {"sbc yl", {0x10}, 1},
    {"sbc ul", {0x20}, 1},
    {"sbc xh", {0x80}, 1},
    {"sbc yh", {0x90}, 1},
    {"sbc uh", {0xA0}, 1},
    {"sbc (x)", {0x01}, 1},
    {"sbc (y)", {0x11}, 1},
    {"sbc (u)", {0x21}, 1},
    {"sbc (0x1234)", {0xA1, 0x12, 0x34}, 3},
    {"sbc #(x)", {0xFD, 0x01}, 2},
    {"sbc #(y)", {0xFD, 0x11}, 2},
    {"sbc #(u)", {0xFD, 0x21}, 2},
    {"sbc #(0x1234)", {0xFD, 0xA1, 0x12, 0x34}, 4},
    {"cpa xl", {0x06}, 1},
    {"cpa yl", {0x16}, 1},
    {"cpa ul", {0x26}, 1},
    {"cpa xh", {0x86}, 1},
    {"cpa yh", {0x96}, 1},
    {"cpa uh", {0xA6}, 1},
    {"cpa (x)", {0x07}, 1},
    {"cpa (y)", {0x17}, 1},
    {"cpa (u)", {0x27}, 1},
    {"cpa (0x1234)", {0xA7, 0x12, 0x34}, 3},
    {"cpa #(x)", {0xFD, 0x07}, 2},
    {"cpa #(y)", {0xFD, 0x17}, 2},
    {"cpa #(u)", {0xFD, 0x27}, 2},
    {"cpa #(0x1234)", {0xFD, 0xA7, 0x12, 0x34}, 4},
    {"lda xl", {0x04}, 1},
    {"lda yl", {0x14}, 1},
    {"lda ul", {0x24}, 1},
    {"lda xh", {0x84}, 1},
    {"lda yh", {0x94}, 1},
    {"lda uh", {0xA4}, 1},
    {"lda (x)", {0x05}, 1},
    {"lda (y)", {0x15}, 1},
    {"lda (u)", {0x25}, 1},
    {"lda (0x1234)", {0xA5, 0x12, 0x34}, 3},
    {"lda #(x)", {0xFD, 0x05}, 2},
    {"lda #(y)", {0xFD, 0x15}, 2},
    {"lda #(u)", {0xFD, 0x25}, 2},
    {"lda #(0x1234)", {0xFD, 0xA5, 0x12, 0x34}, 4},
    {"sta xl", {0x0A}, 1},
    {"sta yl", {0x1A}, 1},
    {"sta ul", {0x2A}, 1},
    {"sta xh", {0x08}, 1},
    {"sta yh", {0x18}, 1},
    {"sta uh", {0x28}, 1},
    {"sta (x)", {0x0E}, 1},
    {"sta (y)", {0x1E}, 1},
    {"sta (u)", {0x2E}, 1},
    {"sta (0x1234)", {0xAE, 0x12, 0x34}, 3},
    {"sta #(x)", {0xFD, 0x0E}, 2},
    {"sta #(y)", {0xFD, 0x1E}, 2},
    {"sta #(u)", {0xFD, 0x2E}, 2},
    {"sta #(0x1234)", {0xFD, 0xAE, 0x12, 0x34}, 4},
    {"and (x)", {0x09}, 1},
    {"and (y)", {0x19}, 1},
    {"and (u)", {0x29}, 1},
    {"and (0x1234)", {0xA9, 0x12, 0x34}, 3},
    {"and #(x)", {0xFD, 0x09}, 2},
    {"and #(y)", {0xFD, 0x19}, 2},
    {"and #(u)", {0xFD, 0x29}, 2},
    {"and #(0x1234)", {0xFD, 0xA9, 0x12, 0x34}, 4},
    {"ora (x)", {0x0B}, 1},
    {"ora (y)", {0x1B}, 1},
    {"ora (u)", {0x2B}, 1},
    {"ora (0x1234)", {0xAB, 0x12, 0x34}, 3},
    {"ora #(x)", {0xFD, 0x0B}, 2},
    {"ora #(y)", {0xFD, 0x1B}, 2},
    {"ora #(u)", {0xFD, 0x2B}, 2},
    {"ora #(0x1234)", {0xFD, 0xAB, 0x12, 0x34}, 4},
    {"eor (x)", {0x0D}, 1},
    {"eor (y)", {0x1D}, 1},
    {"eor (u)", {0x2D}, 1},
    {"eor (0x1234)", {0xAD, 0x12, 0x34}, 3},
    {"eor #(x)", {0xFD, 0x0D}, 2},
    {"eor #(y)", {0xFD, 0x1D}, 2},
    {"eor #(u)", {0xFD, 0x2D}, 2},
    {"eor #(0x1234)", {0xFD, 0xAD, 0x12, 0x34}, 4},
    {"bit (x)", {0x0F}, 1},
    {"bit (y)", {0x1F}, 1},
    {"bit (u)", {0x2F}, 1},
    {"bit (0x1234)", {0xAF, 0x12, 0x34}, 3},
    {"bit #(x)", {0xFD, 0x0F}, 2},
    {"bit #(y)", {0xFD, 0x1F}, 2},
    {"bit #(u)", {0xFD, 0x2F}, 2},
    {"bit #(0x1234)", {0xFD, 0xAF, 0x12, 0x34}, 4},
    {"dca (x)", {0x8C}, 1},
    {"dca (y)", {0x9C}, 1},
    {"dca (u)", {0xAC}, 1},
    {"dca #(x)", {0xFD, 0x8C}, 2},
    {"dca #(y)", {0xFD, 0x9C}, 2},
    {"dca #(u)", {0xFD, 0xAC}, 2},
    {"dcs (x)", {0x0C}, 1},
    {"dcs (y)", {0x1C}, 1},
    {"dcs (u)", {0x2C}, 1},
    {"dcs #(x)", {0xFD, 0x0C}, 2},
    {"dcs #(y)", {0xFD, 0x1C}, 2},
    {"dcs #(u)", {0xFD, 0x2C}, 2},
    {"adi a,0x11", {0xB3, 0x11}, 2},
    {"adi (x),0x11", {0x4F, 0x11}, 2},
    {"adi (y),0x11", {0x5F, 0x11}, 2},
    {"adi (u),0x11", {0x6F, 0x11}, 2},
    {"adi (0x1234),0x11", {0xEF, 0x12, 0x34, 0x11}, 4},
    {"adi #(x),0x11", {0xFD, 0x4F, 0x11}, 3},
    {"adi #(y),0x11", {0xFD, 0x5F, 0x11}, 3},
    {"adi #(u),0x11", {0xFD, 0x6F, 0x11}, 3},
    {"adi #(0x1234),0x11", {0xFD, 0xEF, 0x12, 0x34, 0x11}, 5},
    {"ani a,0x11", {0xB9, 0x11}, 2},
    {"ani (x),0x11", {0x49, 0x11}, 2},
    {"ani (y),0x11", {0x59, 0x11}, 2},
    {"ani (u),0x11", {0x69, 0x11}, 2},
    {"ani (0x1234),0x11", {0xE9, 0x12, 0x34, 0x11}, 4},
    {"ani #(x),0x11", {0xFD, 0x49, 0x11}, 3},
    {"ani #(y),0x11", {0xFD, 0x59, 0x11}, 3},
    {"ani #(u),0x11", {0xFD, 0x69, 0x11}, 3},
    {"ani #(0x1234),0x11", {0xFD, 0xE9, 0x12, 0x34, 0x11}, 5},
    {"ori a,0x11", {0xBB, 0x11}, 2},
    {"ori (x),0x11", {0x4B, 0x11}, 2},
    {"ori (y),0x11", {0x5B, 0x11}, 2},
    {"ori (u),0x11", {0x6B, 0x11}, 2},
    {"ori (0x1234),0x11", {0xEB, 0x12, 0x34, 0x11}, 4},
    {"ori #(x),0x11", {0xFD, 0x4B, 0x11}, 3},
    {"ori #(y),0x11", {0xFD, 0x5B, 0x11}, 3},
    {"ori #(u),0x11", {0xFD, 0x6B, 0x11}, 3},
    {"ori #(0x1234),0x11", {0xFD, 0xEB, 0x12, 0x34, 0x11}, 5},
    {"bii a,0x11", {0xBF, 0x11}, 2},
    {"bii (x),0x11", {0x4D, 0x11}, 2},
    {"bii (y),0x11", {0x5D, 0x11}, 2},
    {"bii (u),0x11", {0x6D, 0x11}, 2},
    {"bii (0x1234),0x11", {0xED, 0x12, 0x34, 0x11}, 4},
    {"bii #(x),0x11", {0xFD, 0x4D, 0x11}, 3},
    {"bii #(y),0x11", {0xFD, 0x5D, 0x11}, 3},
    {"bii #(u),0x11", {0xFD, 0x6D, 0x11}, 3},
    {"bii #(0x1234),0x11", {0xFD, 0xED, 0x12, 0x34, 0x11}, 5},
    {"cpi a,0x11", {0xB7, 0x11}, 2},
    {"cpi xl,0x11", {0x4E, 0x11}, 2},
    {"cpi yl,0x11", {0x5E, 0x11}, 2},
    {"cpi ul,0x11", {0x6E, 0x11}, 2},
    {"cpi xh,0x11", {0x4C, 0x11}, 2},
    {"cpi yh,0x11", {0x5C, 0x11}, 2},
    {"cpi uh,0x11", {0x6C, 0x11}, 2},
    {"sbi a,0x11", {0xB1, 0x11}, 2},
    {"eai a,0x11", {0xBD, 0x11}, 2},
    {"inc a", {0xDD}, 1},
    {"inc xl", {0x40}, 1},
    {"inc yl", {0x50}, 1},
    {"inc ul", {0x60}, 1},
    {"inc xh", {0xFD, 0x40}, 2},
    {"inc yh", {0xFD, 0x50}, 2},
    {"inc uh", {0xFD, 0x60}, 2},
    {"inc x", {0x44}, 1},
    {"inc y", {0x54}, 1},
    {"inc u", {0x64}, 1},
    {"dec a", {0xDF}, 1},
    {"dec xl", {0x42}, 1},
    {"dec yl", {0x52}, 1},
    {"dec ul", {0x62}, 1},
    {"dec xh", {0xFD, 0x42}, 2},
    {"dec yh", {0xFD, 0x52}, 2},
    {"dec uh", {0xFD, 0x62}, 2},
    {"dec x", {0x46}, 1},
    {"dec y", {0x56}, 1},
    {"dec u", {0x66}, 1},
    {"drl (x)", {0xD7}, 1},
    {"drl #(x)", {0xFD, 0xD7}, 2},
    {"drr (x)", {0xD3}, 1},
    {"drr #(x)", {0xFD, 0xD3}, 2},
    {"hlt", {0xFD, 0xB1}, 2},
    {"ita", {0xFD, 0xBA}, 2},
    {"jmp 0x1234", {0xBA, 0x12, 0x34}, 3},
    {"nop", {0x38}, 1},
    {"off", {0xFD, 0x4C}, 2},
    {"rdp", {0xFD, 0xC0}, 2},
    {"rec", {0xF9}, 1},
    {"rie", {0xFD, 0xBE}, 2},
    {"rol", {0xDB}, 1},
    {"ror", {0xD1}, 1},
    {"rpu", {0xE3}, 1},
    {"rpv", {0xB8}, 1},
    {"rti", {0x8A}, 1},
    {"rtn", {0x9A}, 1},
    {"sdp", {0xFD, 0xC1}, 2},
    {"sec", {0xFB}, 1},
    {"shl", {0xD9}, 1},
    {"shr", {0xD5}, 1},
    {"sie", {0xFD, 0x81}, 2},
    {"sjp 0x1234", {0xBE, 0x12, 0x34}, 3},
    {"spu", {0xE1}, 1},
    {"spv", {0xA8}, 1},
    {"tin", {0xF5}, 1},
    {"tta", {0xFD, 0xAA}, 2},
    {"aex", {0xF1}, 1},
    {"am0", {0xFD, 0xCE}, 2},
    {"am1", {0xFD, 0xDE}, 2},
    {"atp", {0xFD, 0xCC}, 2},
    {"att", {0xFD, 0xEC}, 2},
    {"cdv", {0xFD, 0x8E}, 2},
    {"cin", {0xF7}, 1},
    {"ldi xl,0x11", {0x4A, 0x11}, 2},
    {"ldi yl,0x11", {0x5A, 0x11}, 2},
    {"ldi ul,0x11", {0x6A, 0x11}, 2},
    {"ldi xh,0x11", {0x48, 0x11}, 2},
    {"ldi yh,0x11", {0x58, 0x11}, 2},
    {"ldi uh,0x11", {0x68, 0x11}, 2},
    {"ldi a,0x11", {0xB5, 0x11}, 2},
    {"ldi s,0x1234", {0xAA, 0x12, 0x34}, 3},
    {"lde x", {0x47}, 1},
    {"lde y", {0x57}, 1},
    {"lde u", {0x67}, 1},
    {"lin x", {0x45}, 1},
    {"lin y", {0x55}, 1},
    {"lin u", {0x65}, 1},
    {"sde x", {0x43}, 1},
    {"sde y", {0x53}, 1},
    {"sde u", {0x63}, 1},
    {"sin x", {0x41}, 1},
    {"sin y", {0x51}, 1},
    {"sin u", {0x61}, 1},
    {"ldx y", {0xFD, 0x18}, 2},
    {"ldx u", {0xFD, 0x28}, 2},
    {"ldx s", {0xFD, 0x48}, 2},
    {"ldx p", {0xFD, 0x58}, 2},
    {"stx y", {0xFD, 0x5A}, 2},
    {"stx u", {0xFD, 0x6A}, 2},
    {"stx s", {0xFD, 0x4E}, 2},
    {"stx p", {0xFD, 0x5E}, 2},
    {"adr x", {0xFD, 0xCA}, 2},
    {"adr y", {0xFD, 0xDA}, 2},
    {"adr u", {0xFD, 0xEA}, 2},
    {"psh a", {0xFD, 0xC8}, 2},
    {"psh x", {0xFD, 0x88}, 2},
    {"psh y", {0xFD, 0x98}, 2},
    {"psh u", {0xFD, 0xA8}, 2},
    {"pop a", {0xFD, 0x8A}, 2},
    {"pop x", {0xFD, 0x0A}, 2},
    {"pop y", {0xFD, 0x1A}, 2},
    {"pop u", {0xFD, 0x2A}, 2},
    {"vcs 0xC0", {0xC3, 0xC0}, 2},
    {"vcr 0xC0", {0xC1, 0xC0}, 2},
    {"vmj 0xC2", {0xCD, 0xC2}, 2},
    {"vvs 0xC2", {0xCF, 0xC2}, 2},
    {"vzs 0xC2", {0xCB, 0xC2}, 2},
    {"vzr 0xC2", {0xC9, 0xC2}, 2},
    {"vhr 0xC2", {0xC5, 0xC2}, 2},
    {"vhs 0xC2", {0xC7, 0xC2}, 2},
    {"vej 0xC0", {0xC0}, 1},
    {"vej 0xF6", {0xF6}, 1},
    };
    for (const auto& v : kVectors) {
        disasm::Decoded d = disasm::decodeLH5801(0x4000, fetchFrom(v.bytes, 0x4000));
        CHECK_TEXT(d.text, v.text);
        CHECK(d.len == v.len);
        CHECK(!d.illegal);
    }
}

void test_lh5801_branches_and_flow() {
    // The assembler test's hand-computed branches: target = addr + 2 +/- e.
    struct B { uint16_t at; std::vector<uint8_t> bytes; const char* text; uint16_t target; disasm::Flow flow; };
    const B kCases[] = {
        {0x1000, {0x8E, 0x02}, "bch 0x1004", 0x1004, disasm::Flow::Jump},
        {0x1007, {0x9E, 0x02}, "bch 0x1007", 0x1007, disasm::Flow::Jump},
        {0x1009, {0x9E, 0x0B}, "bch 0x1000", 0x1000, disasm::Flow::Jump},
        {0x100B, {0x9B, 0x09}, "bzs 0x1004", 0x1004, disasm::Flow::CondJump},
        {0x1010, {0x88, 0x05}, "lop ul,0x100D", 0x100D, disasm::Flow::CondJump},
        {0x2000, {0xBE, 0xE3, 0x3D}, "sjp 0xE33D", 0xE33D, disasm::Flow::Call},
        {0x2000, {0xBA, 0x12, 0x34}, "jmp 0x1234", 0x1234, disasm::Flow::Jump},
    };
    for (const auto& c : kCases) {
        disasm::Decoded d = disasm::decodeLH5801(c.at, fetchFrom(c.bytes, c.at));
        CHECK_TEXT(d.text, c.text);
        CHECK(d.hasTarget && d.target == c.target);
        CHECK(d.flow == c.flow);
    }
    // Returns, halts, the indirect jump through X
    std::vector<uint8_t> rtn{0x9A}, rti{0x8A}, hlt{0xFD, 0xB1}, stxp{0xFD, 0x5E};
    CHECK(disasm::decodeLH5801(0, fetchFrom(rtn, 0)).flow == disasm::Flow::Return);
    CHECK(disasm::decodeLH5801(0, fetchFrom(rti, 0)).flow == disasm::Flow::Return);
    CHECK(disasm::decodeLH5801(0, fetchFrom(hlt, 0)).flow == disasm::Flow::Halt);
    disasm::Decoded j = disasm::decodeLH5801(0, fetchFrom(stxp, 0));
    CHECK(j.flow == disasm::Flow::Jump && !j.hasTarget);
}

void test_lh5801_vectors_resolve_through_the_table() {
    // VEJ (C4) and VMJ 0x20 read their handler from ME0 FF00+index, big-endian.
    std::vector<uint8_t> mem(65536, 0);
    mem[0xFFC4] = 0xE8; mem[0xFFC5] = 0x9A;
    mem[0xFF20] = 0xD4; mem[0xFF21] = 0x61;
    mem[0x4000] = 0xC4;
    mem[0x4001] = 0xCD; mem[0x4002] = 0x20;
    mem[0x4003] = 0xC3; mem[0x4004] = 0x20;
    auto fetch = [&mem](uint16_t a) { return mem[a]; };
    disasm::Decoded vej = disasm::decodeLH5801(0x4000, fetch);
    CHECK_TEXT(vej.text, "vej 0xC4");
    CHECK(vej.len == 1 && vej.flow == disasm::Flow::Vector && vej.target == 0xE89A);
    disasm::Decoded vmj = disasm::decodeLH5801(0x4001, fetch);
    CHECK_TEXT(vmj.text, "vmj 0x20");
    CHECK(vmj.len == 2 && vmj.flow == disasm::Flow::Vector && vmj.target == 0xD461);
    disasm::Decoded vcs = disasm::decodeLH5801(0x4003, fetch);
    CHECK(vcs.flow == disasm::Flow::CondCall && vcs.target == 0xD461);
}

void test_lh5801_symbols_and_illegal() {
    std::vector<uint8_t> call{0xBE, 0xE3, 0x3D}, load{0xA5, 0x78, 0x65}, bad{0xE5}, badFd{0xFD, 0x00};
    auto symbols = [](uint16_t a) -> std::string { return a == 0xE33D ? "PRINT" : a == 0x7865 ? "WORK" : ""; };
    CHECK_TEXT(disasm::decodeLH5801(0, fetchFrom(call, 0), symbols).text, "sjp PRINT");
    CHECK_TEXT(disasm::decodeLH5801(0, fetchFrom(load, 0), symbols).text, "lda (WORK)");
    disasm::Decoded d = disasm::decodeLH5801(0, fetchFrom(bad, 0));
    CHECK(d.illegal && d.len == 1);
    CHECK_TEXT(d.text, ".db 0xE5");
    disasm::Decoded f = disasm::decodeLH5801(0, fetchFrom(badFd, 0));
    CHECK(f.illegal && f.len == 2);
    CHECK_TEXT(f.text, ".db 0xFD,0x00");
}

// Runs one instruction of `code` at 0x4000 on the real core and returns how
// far P moved and whether the core flagged it undocumented.
struct LhRun { int advance; bool illegal; };
LhRun runLh5801(const std::vector<uint8_t>& code) {
    LhBus bus;
    for (size_t i = 0; i < code.size(); i++) bus.mem[0x4000 + i] = code[i];
    bus.mem[0xFFFE] = 0x40; bus.mem[0xFFFF] = 0x00;
    LH5801 cpu(bus);
    cpu.reset();
    cpu.setSP(0x7000);
    cpu.step();
    return {int(uint16_t(cpu.pc() - 0x4000)), cpu.consumeIllegalOpcodeHit()};
}

void test_lh5801_sweep_matches_cpu() {
    int checked = 0;
    for (int prefix = 0; prefix < 2; prefix++) {
        for (int op = 0; op < 256; op++) {
            std::vector<uint8_t> code;
            if (prefix) code.push_back(0xFD);
            else if (op == 0xFD) continue;
            code.push_back(uint8_t(op));
            code.insert(code.end(), {0x12, 0x34, 0x56, 0x78});
            disasm::Decoded d = disasm::decodeLH5801(0x4000, fetchFrom(code, 0x4000));
            LhRun r = runLh5801(code);
            if (d.illegal != r.illegal)
                std::fprintf(stderr, "  LH5801 %s%02X: decoder illegal=%d, core illegal=%d\n", prefix ? "FD " : "", op, d.illegal, r.illegal);
            CHECK(d.illegal == r.illegal);
            if (d.flow == disasm::Flow::None) {
                if (d.len != r.advance)
                    std::fprintf(stderr, "  LH5801 %s%02X \"%s\": len %d, core advanced %d\n", prefix ? "FD " : "", op, d.text.c_str(), d.len, r.advance);
                CHECK(d.len == r.advance);
                checked++;
            }
        }
    }
    CHECK(checked > 300);
}

void test_lh5801_reset_code_from_rom() {
    // The PC-1500 A04 ROM's reset vector points at E000: rie; ldi a,0; am0; rdp.
    std::vector<uint8_t> rom;
    if (!readRomImage("roms/PC-1500_A04.ROM", &rom) || rom.size() < 0x4000) {
        std::printf("  (skipped: roms/PC-1500_A04.ROM not present)\n");
        return;
    }
    auto fetch = [&rom](uint16_t a) -> uint8_t { return a >= 0xC000 ? rom[a - 0xC000] : 0; };
    const uint16_t reset = uint16_t((fetch(0xFFFE) << 8) | fetch(0xFFFF));
    CHECK(reset == 0xE000);
    const char* const kWant[] = {"rie", "ldi a,0x00", "am0", "rdp"};
    uint16_t pc = reset;
    for (const char* want : kWant) {
        disasm::Decoded d = disasm::decodeLH5801(pc, fetch);
        CHECK_TEXT(d.text, want);
        pc = uint16_t(pc + d.len);
    }
}

// ── Z80 ───────────────────────────────────────────────────────────────────

struct ZVector { uint16_t at; std::vector<uint8_t> bytes; const char* text; int len; };

void test_z80_vectors() {
    static const ZVector kVectors[] = {
        {0x0100, {0x00}, "nop", 1},
        {0x0100, {0x01, 0x34, 0x12}, "ld bc,0x1234", 3},
        {0x0100, {0x08}, "ex af,af'", 1},
        {0x0100, {0x10, 0xFE}, "djnz 0x0100", 2},
        {0x0100, {0x18, 0x00}, "jr 0x0102", 2},
        {0x0100, {0x20, 0x05}, "jr nz,0x0107", 2},
        {0x0100, {0x38, 0xFC}, "jr c,0x00FE", 2},
        {0x0100, {0x22, 0x34, 0x12}, "ld (0x1234),hl", 3},
        {0x0100, {0x2A, 0x34, 0x12}, "ld hl,(0x1234)", 3},
        {0x0100, {0x32, 0x00, 0xF0}, "ld (0xF000),a", 3},
        {0x0100, {0x3A, 0x00, 0xF0}, "ld a,(0xF000)", 3},
        {0x0100, {0x36, 0x55}, "ld (hl),0x55", 2},
        {0x0100, {0x76}, "halt", 1},
        {0x0100, {0x7E}, "ld a,(hl)", 1},
        {0x0100, {0x86}, "add a,(hl)", 1},
        {0x0100, {0x96}, "sub (hl)", 1},
        {0x0100, {0xFE, 0x0D}, "cp 0x0D", 2},
        {0x0100, {0xC3, 0x34, 0x12}, "jp 0x1234", 3},
        {0x0100, {0xCA, 0x34, 0x12}, "jp z,0x1234", 3},
        {0x0100, {0xC9}, "ret", 1},
        {0x0100, {0xD8}, "ret c", 1},
        {0x0100, {0xCD, 0x34, 0x12}, "call 0x1234", 3},
        {0x0100, {0xD3, 0x3C}, "out (0x3C),a", 2},
        {0x0100, {0xDB, 0x3C}, "in a,(0x3C)", 2},
        {0x0100, {0xE3}, "ex (sp),hl", 1},
        {0x0100, {0xE9}, "jp (hl)", 1},
        {0x0100, {0xEB}, "ex de,hl", 1},
        {0x0100, {0xF5}, "push af", 1},
        {0x0100, {0xFF}, "rst 0x0038", 1},
        {0x0100, {0xCB, 0x00}, "rlc b", 2},
        {0x0100, {0xCB, 0x36}, "sll (hl)", 2},
        {0x0100, {0xCB, 0x7E}, "bit 7,(hl)", 2},
        {0x0100, {0xCB, 0xC7}, "set 0,a", 2},
        {0x0100, {0xED, 0x43, 0x34, 0x12}, "ld (0x1234),bc", 4},
        {0x0100, {0xED, 0x7B, 0x34, 0x12}, "ld sp,(0x1234)", 4},
        {0x0100, {0xED, 0x42}, "sbc hl,bc", 2},
        {0x0100, {0xED, 0x7A}, "adc hl,sp", 2},
        {0x0100, {0xED, 0x44}, "neg", 2},
        {0x0100, {0xED, 0x4D}, "reti", 2},
        {0x0100, {0xED, 0x45}, "retn", 2},
        {0x0100, {0xED, 0x5E}, "im 2", 2},
        {0x0100, {0xED, 0x56}, "im 1", 2},
        {0x0100, {0xED, 0x47}, "ld i,a", 2},
        {0x0100, {0xED, 0x78}, "in a,(c)", 2},
        {0x0100, {0xED, 0x70}, "in f,(c)", 2},
        {0x0100, {0xED, 0x71}, "out (c),0", 2},
        {0x0100, {0xED, 0xB0}, "ldir", 2},
        {0x0100, {0xED, 0xBB}, "otdr", 2},
        {0x0100, {0xDD, 0x21, 0x34, 0x12}, "ld ix,0x1234", 4},
        {0x0100, {0xDD, 0x7E, 0x05}, "ld a,(ix+0x05)", 3},
        {0x0100, {0xDD, 0x66, 0xFB}, "ld h,(ix-0x05)", 3},
        {0x0100, {0xDD, 0x74, 0x01}, "ld (ix+0x01),h", 3},
        {0x0100, {0xDD, 0x36, 0x02, 0x99}, "ld (ix+0x02),0x99", 4},
        {0x0100, {0xDD, 0x26, 0x12}, "ld ixh,0x12", 3},
        {0x0100, {0xFD, 0x7D}, "ld a,iyl", 2},
        {0x0100, {0xDD, 0x86, 0x00}, "add a,(ix+0x00)", 3},
        {0x0100, {0xDD, 0x09}, "add ix,bc", 2},
        {0x0100, {0xDD, 0x29}, "add ix,ix", 2},
        {0x0100, {0xDD, 0xE9}, "jp (ix)", 2},
        {0x0100, {0xDD, 0xE3}, "ex (sp),ix", 2},
        {0x0100, {0xDD, 0xEB}, "ex de,hl", 2},
        {0x0100, {0xFD, 0xE5}, "push iy", 2},
        {0x0100, {0xDD, 0x2A, 0x34, 0x12}, "ld ix,(0x1234)", 4},
        {0x0100, {0xDD, 0xCB, 0x03, 0x06}, "rlc (ix+0x03)", 4},
        {0x0100, {0xDD, 0xCB, 0x03, 0x00}, "rlc (ix+0x03),b", 4},
        {0x0100, {0xFD, 0xCB, 0xFE, 0x46}, "bit 0,(iy-0x02)", 4},
        {0x0100, {0xFD, 0xCB, 0x01, 0xFE}, "set 7,(iy+0x01)", 4},
        {0x0100, {0xDD, 0xED, 0xB0}, "ldir", 3},
    };
    for (const auto& v : kVectors) {
        disasm::Decoded d = disasm::decodeZ80(v.at, fetchFrom(v.bytes, v.at));
        CHECK_TEXT(d.text, v.text);
        if (d.len != v.len) std::fprintf(stderr, "  Z80 \"%s\": len %d, want %d\n", v.text, d.len, v.len);
        CHECK(d.len == v.len);
        CHECK(!d.illegal);
    }
}

void test_z80_flow() {
    struct F { std::vector<uint8_t> bytes; disasm::Flow flow; bool hasTarget; uint16_t target; };
    const F kCases[] = {
        {{0xCD, 0x34, 0x12}, disasm::Flow::Call, true, 0x1234},
        {{0xC4, 0x34, 0x12}, disasm::Flow::CondCall, true, 0x1234},
        {{0xEF}, disasm::Flow::Call, true, 0x0028},
        {{0xC9}, disasm::Flow::Return, false, 0},
        {{0xC0}, disasm::Flow::CondReturn, false, 0},
        {{0xED, 0x4D}, disasm::Flow::Return, false, 0},
        {{0xC3, 0x00, 0x80}, disasm::Flow::Jump, true, 0x8000},
        {{0xE9}, disasm::Flow::Jump, false, 0},
        {{0x10, 0x00}, disasm::Flow::CondJump, true, 0x0002},
        {{0x76}, disasm::Flow::Halt, false, 0},
        {{0x3E, 0x01}, disasm::Flow::None, false, 0},
    };
    for (const auto& c : kCases) {
        disasm::Decoded d = disasm::decodeZ80(0, fetchFrom(c.bytes, 0));
        CHECK(d.flow == c.flow);
        CHECK(d.hasTarget == c.hasTarget);
        if (c.hasTarget) CHECK(d.target == c.target);
    }
}

void test_z80_prefix_oddities() {
    std::vector<uint8_t> ddfd{0xDD, 0xFD, 0x21, 0x00, 0x00}, edBad{0xED, 0x00};
    disasm::Decoded d = disasm::decodeZ80(0, fetchFrom(ddfd, 0));
    CHECK(d.illegal && d.len == 1);
    CHECK_TEXT(d.text, ".db 0xDD");
    disasm::Decoded rest = disasm::decodeZ80(1, fetchFrom(ddfd, 0));
    CHECK_TEXT(rest.text, "ld iy,0x0000");
    disasm::Decoded e = disasm::decodeZ80(0, fetchFrom(edBad, 0));
    CHECK(e.illegal && e.len == 2);
    CHECK_TEXT(e.text, ".db 0xED,0x00");
}

// Runs one instruction at 0x4000 on the SC7852 core; returns how far PC moved.
int runZ80(const std::vector<uint8_t>& code) {
    ZBus bus;
    for (size_t i = 0; i < code.size(); i++) bus.mem[0x4000 + i] = code[i];
    SC7852 cpu(bus);
    cpu.reset();
    cpu.setPC(0x4000);
    cpu.setSP(0x7000);
    cpu.setBC(0x0001); // block moves/compares stop after one iteration
    cpu.step();
    return int(uint16_t(cpu.pc() - 0x4000));
}

void test_z80_sweep_matches_cpu() {
    int checked = 0;
    auto check = [&](std::vector<uint8_t> code) {
        // The repeating block ops (ED B0-B3, B8-BB) loop until their
        // counter runs out; their length is 2 either way.
        if (code.size() == 2 && code[0] == 0xED && (code[1] & 0xF4) == 0xB0) return;
        code.insert(code.end(), {0x12, 0x34, 0x56});
        disasm::Decoded d = disasm::decodeZ80(0x4000, fetchFrom(code, 0x4000));
        if (d.flow != disasm::Flow::None) return;
        if (d.illegal && d.len == 1) return; // a dropped DD/FD: the core carries the next prefix instead
        int advance = runZ80(code);
        if (d.len != advance)
            std::fprintf(stderr, "  Z80 %02X %02X %02X %02X \"%s\": len %d, core advanced %d\n",
                         code[0], code[1], code[2], code[3], d.text.c_str(), d.len, advance);
        CHECK(d.len == advance);
        checked++;
    };
    for (int op = 0; op < 256; op++) {
        check({uint8_t(op)});
        check({0xCB, uint8_t(op)});
        check({0xED, uint8_t(op)});
        check({0xDD, uint8_t(op)});
        check({0xFD, uint8_t(op)});
        check({0xDD, 0xCB, 0x05, uint8_t(op)});
    }
    CHECK(checked > 1200);
}

// ── History rings ─────────────────────────────────────────────────────────

void test_lh5801_history_records_bytes_and_post_registers() {
    LhBus bus;
    const uint8_t code[] = {0xB5, 0x42,        // ldi a,0x42
                            0xFD, 0xEF, 0x78, 0x00, 0x01, // adi #(0x7800),0x01 (5 bytes)
                            0x48, 0x78};       // ldi xh,0x78
    for (size_t i = 0; i < sizeof code; i++) bus.mem[0x4000 + i] = code[i];
    bus.mem[0xFFFE] = 0x40; bus.mem[0xFFFF] = 0x00;
    LH5801 cpu(bus);
    cpu.reset();
    CHECK(cpu.traceFlags() == TRACE_NONE);
    CHECK(cpu.history().size() == 0);
    cpu.step(); cpu.step(); cpu.step();
    const auto& h = cpu.history();
    CHECK(h.size() == 3);
    const LH5801HistoryFrame& last = h.recent(0);
    CHECK(last.pc == 0x4007 && last.len == 2 && last.bytes[0] == 0x48 && last.bytes[1] == 0x78);
    CHECK(last.x == 0x7800 && last.p == 0x4009 && last.a == 0x42);
    const LH5801HistoryFrame& adi = h.recent(1);
    CHECK(adi.pc == 0x4002 && adi.len == 5 && adi.bytes[0] == 0xFD && adi.bytes[4] == 0x01);
    CHECK(h.recent(2).pc == 0x4000 && h.recent(2).a == 0x42);
    CHECK(bus.me1[0x7800] == 0x01);
    cpu.reset();
    CHECK(cpu.history().size() == 0);
}

void test_lh5801_history_ring_wraps() {
    LhBus bus;
    for (int i = 0; i < 100; i++) bus.mem[0x4000 + i] = 0x38; // nop
    bus.mem[0xFFFE] = 0x40; bus.mem[0xFFFF] = 0x00;
    LH5801 cpu(bus);
    cpu.reset();
    for (int i = 0; i < 50; i++) cpu.step();
    CHECK(cpu.history().size() == LH5801::History::kSize);
    CHECK(cpu.history().recent(0).pc == 0x4000 + 49);
    CHECK(cpu.history().recent(19).pc == 0x4000 + 30);
}

void test_z80_history_carried_prefix_and_interrupt() {
    ZBus bus;
    const uint8_t code[] = {0xDD, 0xFD, 0x21, 0x34, 0x12, // dropped DD, then ld iy,0x1234
                            0xD9,                         // exx
                            0x00};
    for (size_t i = 0; i < sizeof code; i++) bus.mem[i] = code[i];
    SC7852 cpu(bus);
    cpu.reset();
    cpu.setBC(0xBBCC);
    cpu.step(); cpu.step(); cpu.step();
    const auto& h = cpu.history();
    CHECK(h.size() == 3);
    CHECK(h.recent(2).pc == 0x0000 && h.recent(2).len == 1 && h.recent(2).bytes[0] == 0xDD);
    const Z80HistoryFrame& ld = h.recent(1);
    CHECK(ld.pc == 0x0001 && ld.len == 4 && ld.bytes[0] == 0xFD && ld.bytes[1] == 0x21 && ld.bytes[3] == 0x12);
    CHECK(ld.iy == 0x1234);
    const Z80HistoryFrame& exx = h.recent(0);
    CHECK(exx.bc2 == 0xBBCC && exx.pcAfter == 0x0006);
    // An accepted interrupt leaves an entry of its own.
    bus.mem[0x0006] = 0xFB; // ei
    cpu.step();
    bus.intLine = true;
    cpu.step(); // the EI shadow: executes the nop
    cpu.step(); // accepts: IM 0 behaves as RST 38H
    CHECK(h.recent(0).interrupt && h.recent(0).len == 0 && h.recent(0).pc == 0x0008 && h.recent(0).pcAfter == 0x0038);
    cpu.reset();
    CHECK(cpu.history().size() == 0);
}

void test_z80_history_drops_prefix_before_ed() {
    // DD ED 43 nn nn fetches five bytes; the frame keeps the ED instruction.
    ZBus bus;
    const uint8_t code[] = {0xDD, 0xED, 0x43, 0x00, 0x80};
    for (size_t i = 0; i < sizeof code; i++) bus.mem[i] = code[i];
    SC7852 cpu(bus);
    cpu.reset();
    cpu.setBC(0x1234);
    cpu.step();
    const Z80HistoryFrame& f = cpu.history().recent(0);
    CHECK(f.pc == 0x0001 && f.len == 4);
    CHECK(f.bytes[0] == 0xED && f.bytes[1] == 0x43 && f.bytes[2] == 0x00 && f.bytes[3] == 0x80);
    CHECK(f.pcAfter == 0x0005 && bus.mem[0x8000] == 0x34);
}

} // namespace

int run_disasm_tests() {
    test_lh5801_assembler_vectors();
    test_lh5801_branches_and_flow();
    test_lh5801_vectors_resolve_through_the_table();
    test_lh5801_symbols_and_illegal();
    test_lh5801_sweep_matches_cpu();
    test_lh5801_reset_code_from_rom();
    test_z80_vectors();
    test_z80_flow();
    test_z80_prefix_oddities();
    test_z80_sweep_matches_cpu();
    test_lh5801_history_records_bytes_and_post_registers();
    test_lh5801_history_ring_wraps();
    test_z80_history_carried_prefix_and_interrupt();
    test_z80_history_drops_prefix_before_ed();
    std::printf("disasm tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
