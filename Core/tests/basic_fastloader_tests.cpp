// Headless C++ tests for Core/PC1500/PC1500BasicLoader.cpp -- the fast
// (poke + fix BASPRG_END) BASIC program loader. Needs a real ROM
// (roms/PC-1500_A04.ROM, relative to the repo root); skips (not fails) if
// it is missing, same convention as basictyper_tests.cpp.
//
// The keystroke typer is the oracle: type a program the slow way, capture
// the exact bytes and pointers the ROM produced, wrap them in a CE-158
// transfer file, load that the fast way on a fresh machine, and require a
// byte-identical program area + identical pointers.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../PC1500/PC1500BasicLoader.hpp"
#include "../PC1500/PC1500BasicTyper.hpp"
#include "../PC1500/PC1500Machine.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

bool bootMachine(PC1500Machine& machine) {
    if (!machine.loadROMFile("roms/PC-1500_A04.ROM")) return false;
    machine.reset();
    machine.runCycles(static_cast<uint64_t>(1300000.0 * 2));
    waitIdle(machine, static_cast<uint64_t>(1300000.0 * 5));
    return true;
}

// The loader assumes the preset already NEW0'd -- do it here, the same way
// a `keys:` section would.
void primeNew0(PC1500Machine& machine) {
    tapKey(machine, "cl");
    waitIdle(machine, static_cast<uint64_t>(1300000.0 * 2));
    std::string err;
    typeLine(machine, "NEW0", /*pressEnter=*/true, &err);
    waitIdle(machine, static_cast<uint64_t>(1300000.0 * 2));
}

uint16_t be16(PC1500Machine& m, uint16_t a) {
    return static_cast<uint16_t>((m.memory().peek(a) << 8) |
                                 m.memory().peek(static_cast<uint16_t>(a + 1)));
}

std::vector<uint8_t> readRange(PC1500Machine& m, uint16_t start, uint16_t endExclusive) {
    std::vector<uint8_t> v;
    for (uint16_t a = start; a < endExclusive; a++) v.push_back(m.memory().peek(a));
    return v;
}

std::vector<uint8_t> wrapCe158(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> f(27, 0x00);
    f[0] = 0x01;
    f[1] = 0x40;  // '@' tokenized BASIC
    f[2] = 'C'; f[3] = 'O'; f[4] = 'M';
    const char* name = "ORACLE";
    for (int i = 0; name[i] && i < 16; i++) f[5 + i] = static_cast<uint8_t>(name[i]);
    uint16_t wire = static_cast<uint16_t>(payload.size() - 1);  // capacity-1
    f[0x17] = static_cast<uint8_t>(wire >> 8);
    f[0x18] = static_cast<uint8_t>(wire & 0xFF);
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

// Types `src` through the ROM editor, returns the tokenized program image
// [BASPRG_ST .. BASPRG_END) and fills the pointers seen afterwards.
bool tokenizeViaOracle(const std::string& src, std::vector<uint8_t>* payload, uint16_t* stOut,
                       uint16_t* endOut, uint8_t* markerOut) {
    PC1500Machine m;
    if (!bootMachine(m)) return false;
    BasicTypeResult typed = typeBasicProgramText(m, src);
    if (!typed.ok || !typed.rejectedLines.empty()) return false;
    uint16_t st = be16(m, 0x7865);
    uint16_t end = be16(m, 0x7867);
    if (end < st) return false;
    *payload = readRange(m, st, end);
    *stOut = st;
    *endOut = end;
    *markerOut = m.memory().peek(end);
    return true;
}

void runEquivalenceCase(const char* label, const std::string& src) {
    std::vector<uint8_t> payload;
    uint16_t stA = 0, endA = 0;
    uint8_t markerA = 0;
    if (!tokenizeViaOracle(src, &payload, &stA, &endA, &markerA)) {
        std::fprintf(stderr, "SKIP basic_fastloader %s: ROM not available or typer rejected the program\n",
                     label);
        return;
    }
    CHECK(markerA == 0xFF);                       // typed program ends with a 0xFF
    CHECK(endA == stA + payload.size());

    PC1500Machine m;
    if (!bootMachine(m)) {
        std::fprintf(stderr, "SKIP basic_fastloader %s: roms/PC-1500_A04.ROM not found\n", label);
        return;
    }
    primeNew0(m);
    PC1500BasicLoadResult r = loadBasicBinaryProgram(m, wrapCe158(payload));
    CHECK(r.ok);
    if (!r.ok) {
        std::fprintf(stderr, "  (%s) loader error: %s\n", label, r.error.c_str());
        return;
    }
    CHECK(r.baseAddr == stA);                     // same base as the typed load
    CHECK(r.endAddr == endA);
    CHECK(be16(m, 0x7865) == stA);
    CHECK(be16(m, 0x7867) == endA);               // BASPRG_END written
    CHECK(m.memory().peek(r.endAddr) == 0xFF);    // marker poked

    // Byte-identical program area, marker included.
    std::vector<uint8_t> fast = readRange(m, r.baseAddr, static_cast<uint16_t>(r.endAddr + 1));
    std::vector<uint8_t> want = payload;
    want.push_back(0xFF);
    CHECK(fast == want);
}

void test_equivalence_small() {
    runEquivalenceCase("small", "10 PRINT \"AB\"\n20 GOTO 10\n");
}

void test_equivalence_rem_and_data() {
    runEquivalenceCase("rem+data",
                       "10 REM demo header\n"
                       "20 DATA 1,2,3,4,5\n"
                       "30 FOR I=1 TO 5\n"
                       "40 READ X\n"
                       "50 PRINT X\n"
                       "60 NEXT I\n"
                       "70 END\n");
}

void test_rejects_pc1600_transfer_file() {
    PC1500Machine m;
    if (!bootMachine(m)) {
        std::fprintf(stderr, "SKIP basic_fastloader pc1600-reject: roms/PC-1500_A04.ROM not found\n");
        return;
    }
    // Minimal PC-1600 header (FF 10 00 00, type 0x21) + a one-line payload.
    std::vector<uint8_t> payload = {0x00, 0x0A, 0x03, 0xF1, 0x8E, 0x0D};
    std::vector<uint8_t> f(16, 0x00);
    f[0] = 0xFF; f[1] = 0x10; f[4] = 0x21;
    f[5] = static_cast<uint8_t>(payload.size());
    f[0x0F] = 0x0F;
    f.insert(f.end(), payload.begin(), payload.end());
    PC1500BasicLoadResult r = loadBasicBinaryProgram(m, f);
    CHECK(!r.ok);
    CHECK(r.error.find("PC-1600") != std::string::npos);
}

// A preset that forgot its `- type: NEW0` leaves BASPRG_ST uninitialised
// ($FFFF after a bare boot, or a $00 low byte). The loader must reject that
// with an actionable message rather than poking into system RAM, while
// still accepting the perfectly valid post-NEW0 base $00C5 (16K RAM card
// in the low window).
void test_rejects_missing_new0() {
    std::vector<uint8_t> payload;
    uint16_t st = 0, end = 0;
    uint8_t marker = 0;
    if (!tokenizeViaOracle("10 PRINT \"AB\"\n", &payload, &st, &end, &marker)) {
        std::fprintf(stderr, "SKIP basic_fastloader missing-new0: ROM not available\n");
        return;
    }
    PC1500Machine m;
    if (!bootMachine(m)) {
        std::fprintf(stderr, "SKIP basic_fastloader missing-new0: roms/PC-1500_A04.ROM not found\n");
        return;
    }
    // Deliberately do NOT primeNew0(m).
    PC1500BasicLoadResult r = loadBasicBinaryProgram(m, wrapCe158(payload));
    CHECK(!r.ok);
    CHECK(r.error.find("NEW0") != std::string::npos);
}

void test_rejects_garbage() {
    PC1500Machine m;
    if (!bootMachine(m)) return;
    std::vector<uint8_t> junk(40, 0xAB);
    PC1500BasicLoadResult r = loadBasicBinaryProgram(m, junk);
    CHECK(!r.ok);
}

}  // namespace

int run_basic_fastloader_tests() {
    test_equivalence_small();
    test_equivalence_rem_and_data();
    test_rejects_pc1600_transfer_file();
    test_rejects_missing_new0();
    test_rejects_garbage();

    std::printf("basic_fastloader_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
