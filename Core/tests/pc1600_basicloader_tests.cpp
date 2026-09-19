// Headless C++ tests for Core/PC1600/PC1600BasicLoader.cpp -- the fast
// (poke + fix BASPRG_END) PC-1600 BASIC loader, checked byte-for-byte
// against the keystroke typer. ROM-gated: SKIPs (not fails) when the
// confirmed PC-1600 ROM set is absent, like pc1600_basictyper_tests.cpp.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../PC1600/PC1600BasicLoader.hpp"
#include "../PC1600/PC1600BasicTyper.hpp"
#include "../PC1600/PC1600Machine.hpp"
#include "TestRoms.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// Boots, switches to PRO, and runs NEW0 -- the loadable state the loader
// assumes the preset left the machine in.
bool bootIntoProNew0(PC1600Machine& m) {
    if (!bootPC1600(m)) return false;
    tapKey(m, "mode");  // RUN -> PRO
    waitIdle(m, static_cast<uint64_t>(PC1600Machine::kTStateHz));
    std::string err;
    typeLine(m, "NEW0", /*pressEnter=*/true, &err);
    waitIdle(m, static_cast<uint64_t>(PC1600Machine::kTStateHz));
    return true;
}

// Boot with an N-byte plain RAM module in Slot 1 (32 KB behaves exactly as
// a CE-1600M for placement purposes: unbanked, PVOUT half-split). Attached
// before the boot so the boot ROM folds it into the S0 user area -- NEW0
// then relocates the BASIC program into the module window ($F865 -> $00C5).
bool bootIntoProNew0Slot1Ram(PC1600Machine& m, size_t sizeBytes) {
    if (!m.memory().attachSlot1(sizeBytes)) return false;
    if (!bootPC1600(m)) return false;
    tapKey(m, "mode");  // RUN -> PRO
    waitIdle(m, static_cast<uint64_t>(PC1600Machine::kTStateHz));
    std::string err;
    typeLine(m, "NEW0", /*pressEnter=*/true, &err);
    waitIdle(m, static_cast<uint64_t>(PC1600Machine::kTStateHz));
    return true;
}

uint16_t be16(PC1600Machine& m, uint16_t a) {
    return static_cast<uint16_t>((m.memory().peek(a) << 8) |
                                 m.memory().peek(static_cast<uint16_t>(a + 1)));
}
uint16_t toZ80(uint16_t lh) {
    return (lh >= 0x4000 && lh < 0x8000) ? static_cast<uint16_t>(lh + 0x8000) : lh;
}
std::vector<uint8_t> readRange(PC1600Machine& m, uint16_t s, uint16_t e) {
    std::vector<uint8_t> v;
    for (uint16_t a = s; a < e; a++) v.push_back(m.memory().peek(a));
    return v;
}

std::vector<uint8_t> wrapPc1600(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> f(16, 0x00);
    f[0] = 0xFF; f[1] = 0x10; f[2] = 0x00; f[3] = 0x00;
    f[4] = 0x21;
    uint32_t n = static_cast<uint32_t>(payload.size());
    f[5] = static_cast<uint8_t>(n & 0xFF);
    f[6] = static_cast<uint8_t>((n >> 8) & 0xFF);
    f[7] = static_cast<uint8_t>((n >> 16) & 0xFF);
    f[0x0E] = 0x00; f[0x0F] = 0x0F;
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

void test_equivalence_against_typer() {
    std::vector<uint8_t> payload;
    uint16_t baseA = 0, endA = 0;
    {
        PC1600Machine m;
        if (!bootIntoProNew0(m)) {
            std::fprintf(stderr, "SKIP pc1600_basicloader: PC-1600 ROM set not found\n");
            return;
        }
        PC1600BasicTypeResult typed =
            typeBasicProgramText(m, "10 REM header\n20 PRINT \"AB\"\n30 GOTO 20\n40 END\n");
        CHECK(typed.ok);
        CHECK(typed.rejectedLines.empty());
        baseA = toZ80(be16(m, 0xF865));
        endA = toZ80(be16(m, 0xF867));
        CHECK(endA > baseA);
        payload = readRange(m, baseA, endA);
        CHECK(m.memory().peek(endA) == 0xFF);
    }

    PC1600Machine m;
    if (!bootIntoProNew0(m)) return;
    PC1600BasicLoadResult r = loadBasicBinaryProgram(m, wrapPc1600(payload));
    CHECK(r.ok);
    if (!r.ok) {
        std::fprintf(stderr, "  loader error: %s\n", r.error.c_str());
        return;
    }
    CHECK(r.baseAddr == baseA);
    CHECK(r.endAddr == endA);
    CHECK(toZ80(be16(m, 0xF867)) == endA);   // BASPRG_END written (LH5803 form)
    CHECK(m.memory().peek(r.endAddr) == 0xFF);

    std::vector<uint8_t> fast = readRange(m, r.baseAddr, static_cast<uint16_t>(r.endAddr + 1));
    std::vector<uint8_t> want = payload;
    want.push_back(0xFF);
    CHECK(fast == want);
}

// Run `RUN`, let it settle, return the VARIABLE POINTER ($F899, BE). A
// program that assigns a variable moves it; a program that errors out at
// RUN time doesn't -- so it doubles as "did the interpreter actually
// execute the program".
uint16_t runAndReadVarPtr(PC1600Machine& m) {
    tapKey(m, "mode");  // PRO -> RUN
    waitIdle(m, static_cast<uint64_t>(PC1600Machine::kTStateHz));
    std::string err;
    typeLine(m, "RUN", /*pressEnter=*/true, &err);
    m.runCycles(static_cast<uint64_t>(PC1600Machine::kTStateHz) * 3);
    waitIdle(m, static_cast<uint64_t>(PC1600Machine::kTStateHz) * 3);
    return be16(m, 0xF899);
}

// The generalized-placement case: a Slot-1 RAM module has pushed the BASIC
// program area into the module window. The fast loader must scatter the
// tokenised image into the module's backing store byte-for-byte the way
// the keystroke typer's stored program looks, and the result must RUN.
void test_ce1600m_module_equivalence_and_run() {
    const char* src = "10 A=42\n20 B=A+1\n30 END\n";

    // Oracle: type the program with the module fitted; read the stored
    // tokens straight out of the module backing store.
    std::vector<uint8_t> typedPayload;
    uint16_t typedStLh = 0, typedEndLh = 0, typedVarPtrAfterRun = 0;
    {
        PC1600Machine m;
        if (!bootIntoProNew0Slot1Ram(m, 0x8000)) {
            std::fprintf(stderr, "SKIP pc1600_basicloader ce1600m: PC-1600 ROM set not found\n");
            return;
        }
        PC1600BasicTypeResult typed = typeBasicProgramText(m, src);
        CHECK(typed.ok);
        CHECK(typed.rejectedLines.empty());
        typedStLh = be16(m, 0xF865);
        typedEndLh = be16(m, 0xF867);
        CHECK(typedStLh == 0x00C5);           // relocated into the module window
        CHECK(typedEndLh > typedStLh);
        std::vector<uint8_t> img = m.debugSlotImage(1);
        CHECK(img.size() == 0x8000);
        if (img.size() == 0x8000 && typedEndLh > typedStLh && typedEndLh < 0x8000) {
            typedPayload.assign(img.begin() + typedStLh, img.begin() + typedEndLh);
            CHECK(img[typedEndLh] == 0xFF);
        }
        typedVarPtrAfterRun = runAndReadVarPtr(m);
    }
    CHECK(!typedPayload.empty());
    if (typedPayload.empty()) return;

    // Fast path: load that payload into a fresh module machine.
    PC1600Machine m;
    if (!bootIntoProNew0Slot1Ram(m, 0x8000)) return;
    PC1600BasicLoadResult r = loadBasicBinaryPayload(m, typedPayload);
    CHECK(r.ok);
    if (!r.ok) {
        std::fprintf(stderr, "  loader error: %s\n", r.error.c_str());
        return;
    }
    CHECK(r.baseAddr == static_cast<uint16_t>(typedStLh + 0x8000));   // Z-80 $80C5
    CHECK(r.endAddr == static_cast<uint16_t>(typedEndLh + 0x8000));
    CHECK(be16(m, 0xF867) == static_cast<uint16_t>(typedStLh + typedPayload.size()));

    std::vector<uint8_t> img = m.debugSlotImage(1);
    std::vector<uint8_t> want = typedPayload;
    want.push_back(0xFF);
    std::vector<uint8_t> got(img.begin() + typedStLh,
                             img.begin() + typedStLh + typedPayload.size() + 1);
    CHECK(got == want);   // byte-for-byte == the keystroke typer's stored program

    // ... and it RUNs: the interpreter reads the scattered program and
    // lands in exactly the state the typed program's RUN produced (same
    // BASPRG_END, same VARIABLE POINTER) -- i.e. the fast-loaded program
    // behaves identically to a correctly-stored one.
    uint16_t varPtrAfter = runAndReadVarPtr(m);
    CHECK(varPtrAfter == typedVarPtrAfterRun);
    CHECK(be16(m, 0xF867) == static_cast<uint16_t>(typedStLh + typedPayload.size()));

    // The program survived RUN in the module backing store.
    std::vector<uint8_t> post = m.debugSlotImage(1);
    std::vector<uint8_t> afterRun(post.begin() + typedStLh,
                                  post.begin() + typedStLh + typedPayload.size() + 1);
    CHECK(afterRun == want);
}

// Reloading over a resident program must fully erase its tail rather than
// just moving BASPRG_END back -- otherwise a shorter reload leaves stale
// tokens dangling in RAM (harmless to RUN, but a stray dump of that area
// would show leftover garbage from the previous program).
void test_reload_over_shorter_program_clears_tail_stock() {
    PC1600Machine m;
    if (!bootIntoProNew0(m)) {
        std::fprintf(stderr, "SKIP pc1600_basicloader reload-shorter: PC-1600 ROM set not found\n");
        return;
    }
    std::vector<uint8_t> longPayload = {0x00, 0x0A, 0x03, 0xF1, 0x22, 0x48, 0x45, 0x4C, 0x4C, 0x4F,
                                       0x22, 0x0D};
    std::vector<uint8_t> shortPayload = {0x00, 0x0A, 0x03, 0xF1, 0x30, 0x0D};
    CHECK(longPayload.size() > shortPayload.size());

    PC1600BasicLoadResult first = loadBasicBinaryPayload(m, longPayload);
    CHECK(first.ok);
    if (!first.ok) {
        std::fprintf(stderr, "  loader error: %s\n", first.error.c_str());
        return;
    }
    uint16_t oldEnd = first.endAddr;

    PC1600BasicLoadResult second = loadBasicBinaryPayload(m, shortPayload);
    CHECK(second.ok);
    if (!second.ok) {
        std::fprintf(stderr, "  loader error: %s\n", second.error.c_str());
        return;
    }
    CHECK(second.baseAddr == first.baseAddr);   // same BASPRG_ST, no NEW involved
    CHECK(second.endAddr < oldEnd);
    CHECK(toZ80(be16(m, 0xF867)) == second.endAddr);

    for (uint16_t a = static_cast<uint16_t>(second.endAddr + 1); a <= oldEnd; a++) {
        CHECK(m.memory().peek(a) == 0x00);
    }
}

// A live BASPRG_END that doesn't sit at/after BASPRG_ST must be rejected --
// the loader relies on it to know how much of the resident program to erase.
void test_rejects_invalid_basprg_end() {
    PC1600Machine m;
    if (!bootIntoProNew0(m)) {
        std::fprintf(stderr, "SKIP pc1600_basicloader invalid-end: PC-1600 ROM set not found\n");
        return;
    }
    uint16_t st = be16(m, 0xF865);
    uint16_t badEnd = static_cast<uint16_t>(st - 1);
    m.memory().poke(0xF867, static_cast<uint8_t>(badEnd >> 8));
    m.memory().poke(0xF868, static_cast<uint8_t>(badEnd & 0xFF));

    std::vector<uint8_t> payload = {0x00, 0x0A, 0x03, 0xF1, 0x30, 0x0D};
    PC1600BasicLoadResult r = loadBasicBinaryPayload(m, payload);
    CHECK(!r.ok);
    CHECK(r.error.find("BASPRG_END") != std::string::npos);
}

// Same as test_reload_over_shorter_program_clears_tail_stock, but with a
// Slot-1 RAM module fitted, so the vacated tail lives in the module's
// backing store rather than internal RAM -- the erase plan must still find
// and clear it via the same scattered-placement logic used for writes.
void test_reload_over_shorter_program_clears_tail_module() {
    PC1600Machine m;
    if (!bootIntoProNew0Slot1Ram(m, 0x8000)) {
        std::fprintf(stderr, "SKIP pc1600_basicloader reload-shorter-module: PC-1600 ROM set not found\n");
        return;
    }
    std::vector<uint8_t> longPayload = {0x00, 0x0A, 0x03, 0xF1, 0x22, 0x48, 0x45, 0x4C, 0x4C, 0x4F,
                                       0x22, 0x0D};
    std::vector<uint8_t> shortPayload = {0x00, 0x0A, 0x03, 0xF1, 0x30, 0x0D};

    PC1600BasicLoadResult first = loadBasicBinaryPayload(m, longPayload);
    CHECK(first.ok);
    if (!first.ok) {
        std::fprintf(stderr, "  loader error: %s\n", first.error.c_str());
        return;
    }
    uint16_t oldEndLh = be16(m, 0xF867);

    PC1600BasicLoadResult second = loadBasicBinaryPayload(m, shortPayload);
    CHECK(second.ok);
    if (!second.ok) {
        std::fprintf(stderr, "  loader error: %s\n", second.error.c_str());
        return;
    }
    uint16_t newEndLh = be16(m, 0xF867);
    CHECK(newEndLh < oldEndLh);

    // Both ends land inside the module window here (0x00C5-based, well below
    // the module's 32K size), so the vacated tail is entirely in the
    // module's own backing store.
    std::vector<uint8_t> img = m.debugSlotImage(1);
    CHECK(!img.empty());
    if (img.empty() || oldEndLh >= img.size()) return;
    for (uint16_t a = static_cast<uint16_t>(newEndLh + 1); a <= oldEndLh; a++) {
        CHECK(img[a] == 0x00);
    }
}

void test_rejects_pc1500_transfer_file() {
    PC1600Machine m;
    if (!bootIntoProNew0(m)) {
        std::fprintf(stderr, "SKIP pc1600_basicloader pc1500-reject: PC-1600 ROM set not found\n");
        return;
    }
    // CE-158 header (0x01 .. "COM", type 0x40) + a one-line payload.
    std::vector<uint8_t> payload = {0x00, 0x0A, 0x03, 0xF1, 0x8E, 0x0D};
    std::vector<uint8_t> f(27, 0x00);
    f[0] = 0x01; f[1] = 0x40; f[2] = 'C'; f[3] = 'O'; f[4] = 'M';
    uint16_t wire = static_cast<uint16_t>(payload.size() - 1);
    f[0x17] = static_cast<uint8_t>(wire >> 8);
    f[0x18] = static_cast<uint8_t>(wire & 0xFF);
    f.insert(f.end(), payload.begin(), payload.end());
    PC1600BasicLoadResult r = loadBasicBinaryProgram(m, f);
    CHECK(!r.ok);
    CHECK(r.error.find("PC-1500") != std::string::npos);
}

}  // namespace

int run_pc1600_basicloader_tests() {
    test_equivalence_against_typer();
    test_ce1600m_module_equivalence_and_run();
    test_reload_over_shorter_program_clears_tail_stock();
    test_reload_over_shorter_program_clears_tail_module();
    test_rejects_invalid_basprg_end();
    test_rejects_pc1500_transfer_file();

    std::printf("pc1600_basicloader_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
