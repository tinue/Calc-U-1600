// Headless tests for "Load Machine Code…": Core/MachineCodeFile (header
// recognition, the load plan, the NEW/CALL advice, hex-address parsing)
// and the two writers, PC1500MachineCodeLoader / PC1600MachineCodeLoader.
// Same no-framework, assert-and-tally style as the other Core test files.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../MachineCodeFile.hpp"
#include "../PC1500/PC1500MachineCodeLoader.hpp"
#include "../PC1500/PC1500Machine.hpp"
#include "../PC1600/PC1600MachineCodeLoader.hpp"
#include "../PC1600/PC1600BasicTyper.hpp"
#include "../PC1600/PC1600Machine.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using machinecode::File;
using machinecode::Slot;
using machinecode::Target;

const std::vector<uint8_t> kCode = {0x3E, 0x41, 0xC9, 0x00, 0x11};  // 5 bytes

std::vector<uint8_t> pc1600File(const std::vector<uint8_t>& payload, uint32_t load, uint32_t autorun,
                                uint8_t type = 0x10) {
    std::vector<uint8_t> f = {0xFF, 0x10, 0x00, 0x00, type};
    const uint32_t n = static_cast<uint32_t>(payload.size());
    for (uint32_t v : {n, load, autorun}) {
        f.push_back(static_cast<uint8_t>(v & 0xFF));
        f.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        f.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    }
    f.push_back(0x00);
    f.push_back(0x0F);
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

std::vector<uint8_t> ce158File(const std::vector<uint8_t>& payload, uint16_t load, uint16_t autorun,
                               uint8_t type = 0x42) {
    std::vector<uint8_t> f = {0x01, type, 'C', 'O', 'M'};
    const char name[16] = "TEST";
    f.insert(f.end(), name, name + 16);
    const uint16_t lenField = static_cast<uint16_t>(payload.size() - 1);  // stored as length - 1
    for (uint16_t v : {load, lenField, autorun}) {
        f.push_back(static_cast<uint8_t>(v >> 8));
        f.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

// ── readFile ─────────────────────────────────────────────────────────────

void test_read_ce158() {
    File f = machinecode::readFile(ce158File(kCode, 0x40C5, 0x40C7));
    CHECK(f.ok);
    CHECK(f.header == File::Header::CE158);
    CHECK(f.loadAddr == 0x40C5);
    CHECK(f.autorunAddr == 0x40C7);
    CHECK(f.payload == kCode);
}

void test_read_ce158_length_mismatch() {
    auto bytes = ce158File(kCode, 0x40C5, 0);
    bytes.push_back(0xAA);  // one byte more than the header's (length - 1) + 1
    File f = machinecode::readFile(bytes);
    CHECK(!f.ok);
    CHECK(f.header == File::Header::CE158);
    CHECK(!f.error.empty());
}

void test_read_ce158_basic_type_rejected() {
    File f = machinecode::readFile(ce158File(kCode, 0, 0, /*type=*/0x40));
    CHECK(!f.ok);
    CHECK(f.error.find("not machine code") != std::string::npos);
}

void test_read_pc1600() {
    File f = machinecode::readFile(pc1600File(kCode, 0xC0C5, 0));
    CHECK(f.ok);
    CHECK(f.header == File::Header::PC1600);
    CHECK(f.loadAddr == 0xC0C5);
    CHECK(f.autorunAddr == 0);
    CHECK(f.payload == kCode);
}

void test_read_pc1600_basic_type_rejected() {
    File f = machinecode::readFile(pc1600File(kCode, 0, 0, /*type=*/0x21));
    CHECK(!f.ok);
    CHECK(f.error.find("Load BASIC Program") != std::string::npos);
}

void test_read_headerless() {
    File f = machinecode::readFile(kCode);
    CHECK(f.ok);
    CHECK(f.header == File::Header::None);
    CHECK(f.payload == kCode);
}

// ── plan ─────────────────────────────────────────────────────────────────

void test_plan_model_mismatch() {
    auto ce158 = machinecode::readFile(ce158File(kCode, 0x40C5, 0));
    CHECK(!machinecode::plan(Target::PC1600, ce158, true, true).error.empty());
    CHECK(machinecode::plan(Target::PC1500, ce158, false, false).error.empty());

    auto pc1600 = machinecode::readFile(pc1600File(kCode, 0xC0C5, 0));
    CHECK(!machinecode::plan(Target::PC1500, pc1600, false, false).error.empty());
    CHECK(machinecode::plan(Target::PC1600, pc1600, false, false).error.empty());
}

void test_plan_headerless_needs_address() {
    auto f = machinecode::readFile(kCode);
    auto p1500 = machinecode::plan(Target::PC1500, f, false, false);
    CHECK(p1500.error.empty() && p1500.needsAddress);
    auto p1600 = machinecode::plan(Target::PC1600, f, true, true);
    CHECK(p1600.error.empty() && p1600.needsAddress);
}

void test_plan_pc1600_slot_choices() {
    auto s0 = machinecode::readFile(pc1600File(kCode, 0xC0C5, 0));
    auto p = machinecode::plan(Target::PC1600, s0, true, true);
    CHECK(p.slotChoices.size() == 1 && p.slotChoices[0] == Slot::S0);

    auto slot = machinecode::readFile(pc1600File(kCode, 0x80C5, 0));
    CHECK(!machinecode::plan(Target::PC1600, slot, false, false).error.empty());
    p = machinecode::plan(Target::PC1600, slot, false, true);
    CHECK(p.slotChoices.size() == 1 && p.slotChoices[0] == Slot::S2);
    p = machinecode::plan(Target::PC1600, slot, true, true);
    CHECK(p.slotChoices.size() == 2 && p.slotChoices[0] == Slot::S1 && p.slotChoices[1] == Slot::S2);

    auto rom = machinecode::readFile(pc1600File(kCode, 0x7000, 0));
    CHECK(!machinecode::plan(Target::PC1600, rom, true, true).error.empty());

    // $BFFE + 5 bytes crosses into $C000.
    auto crossing = machinecode::readFile(pc1600File(kCode, 0xBFFE, 0));
    CHECK(!machinecode::plan(Target::PC1600, crossing, true, true).error.empty());
}

// ── advice ───────────────────────────────────────────────────────────────

void test_advice_pc1500() {
    // PC-1500A-style RAM $4000-$57FF: BASIC can start at $40C5 at the lowest.
    auto a = machinecode::advice(Target::PC1500, Slot::S0, 0x40C5, 0x20, 0, 0x4000, 0x5800, {});
    CHECK(a.newCommand == "NEW &40E5");
    CHECK(a.callCommand == "CALL &40C5");
    CHECK(a.callNote.find("first byte") != std::string::npos);

    a = machinecode::advice(Target::PC1500, Slot::S0, 0x4010, 0x20, 0x4012, 0x4000, 0x5800, {});
    CHECK(a.newCommand.empty());
    CHECK(a.newNote.find("reserve") != std::string::npos);
    CHECK(a.callCommand == "CALL &4012");
    CHECK(a.callNote.find("auto-run") != std::string::npos);

    a = machinecode::advice(Target::PC1500, Slot::S0, 0x7C01, 0x10, 0, 0x4000, 0x5800, {});
    CHECK(a.newCommand.empty());
    CHECK(a.newNote.find("outside") != std::string::npos);
}

void test_advice_pc1600() {
    using machinecode::BasicArea;
    const std::vector<BasicArea> stock = {{0, 0xC000, 0xEFFF, 0}};
    const std::vector<BasicArea> slot1First = {{1, 0x8000, 0xBFFF, 0}, {0, 0xC000, 0xEFFF, 0}};
    const std::vector<BasicArea> slot2First = {{2, 0x8000, 0xBFFF, 0}, {0, 0xC000, 0xEFFF, 0}};

    // Stock: the S0 area starts in internal RAM.
    auto a = machinecode::advice(Target::PC1600, Slot::S0, 0xC0C5, 0x100, 0, 0, 0, stock);
    CHECK(a.newCommand == "NEW \"S0:\",&1C5");
    CHECK(a.newNote.find("Warning") == std::string::npos);
    CHECK(a.callCommand == "CALL &C0C5");

    a = machinecode::advice(Target::PC1600, Slot::S0, 0xC000, 0x10, 0, 0, 0, stock);
    CHECK(a.newNote.find("Warning") != std::string::npos);

    a = machinecode::advice(Target::PC1600, Slot::S0, 0xEFF0, 0x20, 0, 0, 0, stock);
    CHECK(a.newNote.find("work area") != std::string::npos);

    // Module in slot 1: BASIC starts there, so NEW "S0:" counts from $8000.
    a = machinecode::advice(Target::PC1600, Slot::S1, 0x80C5, 0x40, 0x80D0, 0, 0, slot1First);
    CHECK(a.newCommand == "NEW \"S0:\",&105");
    CHECK(a.callCommand == "CALL &80D0");

    // ... and internal RAM is the area's LAST run: NEW can't protect code there.
    a = machinecode::advice(Target::PC1600, Slot::S0, 0xC0C5, 0x40, 0, 0, 0, slot1First);
    CHECK(a.newCommand.empty());
    CHECK(a.newNote.find("&80C5") != std::string::npos);

    // Slot 2 outside the BASIC area: no NEW needed; CALL goes through bank 2.
    a = machinecode::advice(Target::PC1600, Slot::S2, 0x80C5, 0x40, 0, 0, 0, slot1First);
    CHECK(a.newCommand.empty());
    CHECK(a.newNote.find("doesn't use") != std::string::npos);
    CHECK(a.callCommand == "CALL #2,&80C5");

    a = machinecode::advice(Target::PC1600, Slot::S2, 0x80C5, 0x40, 0, 0, 0, slot2First);
    CHECK(a.newCommand == "NEW \"S0:\",&105");
    CHECK(a.callCommand == "CALL #2,&80C5");

    a = machinecode::advice(Target::PC1600, Slot::S0, 0xC0C5, 0x40, 0, 0, 0, {});
    CHECK(a.newCommand.empty());
    CHECK(a.newNote.find("couldn't be read") != std::string::npos);
}

// ── parseHexAddress ──────────────────────────────────────────────────────

void test_parse_hex_address() {
    uint32_t v = 0;
    CHECK(machinecode::parseHexAddress("C0C5", &v) && v == 0xC0C5);
    CHECK(machinecode::parseHexAddress(" &c0c5 ", &v) && v == 0xC0C5);
    CHECK(machinecode::parseHexAddress("$40C5", &v) && v == 0x40C5);
    CHECK(machinecode::parseHexAddress("0x7C01", &v) && v == 0x7C01);
    CHECK(!machinecode::parseHexAddress("", &v));
    CHECK(!machinecode::parseHexAddress("&", &v));
    CHECK(!machinecode::parseHexAddress("12G4", &v));
    CHECK(!machinecode::parseHexAddress("10000", &v));
}

// ── writers ──────────────────────────────────────────────────────────────

void test_pc1600_writer() {
    PC1600Machine m;
    std::string err;
    CHECK(loadPC1600MachineCode(m, 0, 0xC0C5, kCode.data(), kCode.size(), &err));
    std::vector<uint8_t> ram(PC1600Machine::kInternalRamSize);
    m.debugCopyInternalRam(ram.data());
    CHECK(std::vector<uint8_t>(ram.begin() + 0xC5, ram.begin() + 0xC5 + 5) == kCode);

    // Slot 1: nothing attached -> refused; with 32 KB RAM -> lands at image offset $00C5.
    CHECK(!loadPC1600MachineCode(m, 1, 0x80C5, kCode.data(), kCode.size(), &err));
    CHECK(m.memory().attachSlot1(0x8000));
    CHECK(loadPC1600MachineCode(m, 1, 0x80C5, kCode.data(), kCode.size(), &err));
    std::vector<uint8_t> image = m.debugSlotImage(1);
    CHECK(image.size() >= 0xC5 + 5);
    if (image.size() >= 0xC5 + 5) CHECK(std::vector<uint8_t>(image.begin() + 0xC5, image.begin() + 0xC5 + 5) == kCode);

    // Window checks.
    CHECK(!loadPC1600MachineCode(m, 0, 0xBFFF, kCode.data(), kCode.size(), &err));
    CHECK(!loadPC1600MachineCode(m, 1, 0xBFFE, kCode.data(), kCode.size(), &err));
}

void test_pc1500_writer() {
    PC1500Machine m;
    CHECK(m.loadROMFile("roms/PC-1500_A04.ROM"));
    std::string err;
    CHECK(loadPC1500MachineCode(m, 0x40C5, kCode.data(), kCode.size(), &err));
    for (size_t i = 0; i < kCode.size(); i++) CHECK(m.memory().peek(static_cast<uint16_t>(0x40C5 + i)) == kCode[i]);

    // System ROM at $C000: writes are dropped -> reported.
    CHECK(!loadPC1500MachineCode(m, 0xC000, kCode.data(), kCode.size(), &err));
    CHECK(err.find("not RAM") != std::string::npos);

    CHECK(!loadPC1500MachineCode(m, 0xFFFE, kCode.data(), kCode.size(), &err));
}

bool readFileBytes(const char* path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    *out = std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return !out->empty();
}

bool bootPC1600(PC1600Machine& m, size_t slot1Bytes) {
    std::vector<uint8_t> i0, ii0, iii3, r3b, iv6, r1500;
    if (!readFileBytes("roms/PC1600-P0-B0.bin", &i0) || !readFileBytes("roms/PC1600-P1-B0.bin", &ii0) ||
        !readFileBytes("roms/PC1600-P1-B3.bin", &iii3) || !readFileBytes("roms/PC1600-P1-B3B.bin", &r3b) ||
        !readFileBytes("roms/PC1600-P2-B6.bin", &iv6) ||
        !readFileBytes("roms/PC1600-LH5803-C000-FFFF.bin", &r1500))
        return false;
    if (!m.loadBank0(i0.data(), i0.size(), ii0.data(), ii0.size()) || !m.loadBank3Rom(iii3.data(), iii3.size()) ||
        !m.loadBank3bRom(r3b.data(), r3b.size()) || !m.loadBank6Rom(iv6.data(), iv6.size()) ||
        !m.loadLH5803Rom(r1500.data(), r1500.size()))
        return false;
    if (slot1Bytes && !m.memory().attachSlot1(slot1Bytes)) return false;
    m.allReset();
    m.runCycles(static_cast<uint64_t>(PC1600Machine::kTStateHz) * 2);
    waitIdle(m, static_cast<uint64_t>(PC1600Machine::kTStateHz) * 5);
    return true;
}

void test_pc1600_basic_areas() {
    {
        PC1600Machine m;
        CHECK(bootPC1600(m, 0));
        auto areas = pc1600BasicAreas(m);
        CHECK(areas.size() == 1);
        if (!areas.empty()) CHECK(areas[0].slot == 0 && areas[0].windowBase == 0xC000);
    }
    {
        PC1600Machine m;
        CHECK(bootPC1600(m, 0x8000));  // 32 KB RAM module in slot 1, folded into S0 at boot
        auto areas = pc1600BasicAreas(m);
        CHECK(areas.size() >= 2);
        if (areas.size() >= 2) {
            CHECK(areas.front().slot == 1 && areas.front().windowBase == 0x8000);
            CHECK(areas.front().imageOffset == 0);
            CHECK(areas.back().slot == 0);
        }
    }
}

}  // namespace

int run_machine_code_file_tests() {
    test_read_ce158();
    test_read_ce158_length_mismatch();
    test_read_ce158_basic_type_rejected();
    test_read_pc1600();
    test_read_pc1600_basic_type_rejected();
    test_read_headerless();
    test_plan_model_mismatch();
    test_plan_headerless_needs_address();
    test_plan_pc1600_slot_choices();
    test_advice_pc1500();
    test_advice_pc1600();
    test_parse_hex_address();
    test_pc1600_writer();
    test_pc1500_writer();
    test_pc1600_basic_areas();

    std::printf("machine_code_file_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
