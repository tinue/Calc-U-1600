// Headless C++ tests for the CE-150 printer/plotter/cassette interface:
// Ce150Card's LH5810 register + ROM-window decode, its use of the shared
// AlpsPlotterMechanism, and its integration into PC1500Machine over the
// 60-pin SystemBus (including the ME1 0xB00x shadow of the internal
// LH5811). Same no-framework, assert-and-tally style as lh5801_tests.cpp.
//
// Build & run: see tools/run_tests.sh

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "../Connector/Ce150Card.hpp"
#include "../PC1500/PC1500Machine.hpp"
#include "../PC1500/PC1500PresetLoader.hpp"
#include "../PC1500/PresetFile.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// cmotor.cpp's 8-position drive ring (see AlpsPlotterMechanism / ce1600p_tests):
// adjacent entries are a half-step (+/-1) in the X motor's own convention.
constexpr uint8_t kRing[8] = {0x9, 0x8, 0xc, 0x4, 0x6, 0x2, 0x3, 0x1};

PinState me1Read(uint16_t addr) {
    PinState p; p.address = addr; p.forWrite = false; p.me1 = true; return p;
}
PinState me1Write(uint16_t addr) {
    PinState p; p.address = addr; p.forWrite = true; p.me1 = true; return p;
}
PinState me0Read(uint16_t addr, bool pv = false) {
    PinState p; p.address = addr; p.forWrite = false; p.me1 = false;
    p.pin[2] = pv;                 // PV
    p.pin[19] = (addr >= 0x8000 && addr < 0xC000); // Y2, as SystemBus::decode sets it
    return p;
}

std::vector<uint8_t> fakeRom() {
    std::vector<uint8_t> rom(Ce150Card::kRomSize);
    for (size_t i = 0; i < rom.size(); ++i) rom[i] = uint8_t(i * 7 + 3);
    return rom;
}

// ── ROM window ────────────────────────────────────────────────────────

void test_rom_window_decode_and_negatives() {
    Ce150Card card;
    auto rom = fakeRom();
    CHECK(card.loadRom(rom.data(), rom.size()));
    CHECK(!card.loadRom(rom.data(), rom.size() - 1)); // wrong size rejected

    uint8_t v = 0;
    PinState p = me0Read(0xA000);
    CHECK(card.respondsToRead(p, v) && v == rom[0]);
    p = me0Read(0xBFFF);
    CHECK(card.respondsToRead(p, v) && v == rom[0x1FFF]);

    // Not the ROM: PV high, a write, an ME1 access, or an address outside
    // 0xA000-0xBFFF all decline the ROM window.
    p = me0Read(0xA000, /*pv=*/true);
    CHECK(!card.respondsToRead(p, v));
    p = me0Read(0xA000); p.forWrite = true;
    CHECK(!card.respondsToWrite(p, 0x55)); // ROM is read-only
    p = me0Read(0x9FFF);
    CHECK(!card.respondsToRead(p, v));
    p = me0Read(0xC000);
    CHECK(!card.respondsToRead(p, v));
}

// ── LH5810 register block ─────────────────────────────────────────────

void test_lh5810_latches_and_direction_masking() {
    Ce150Card card;
    uint8_t v = 0;

    // G / MSK / IF / DDA / DDB are plain read/write latches.
    for (uint16_t addr : {uint16_t(0xB009), uint16_t(0xB00A), uint16_t(0xB00B),
                          uint16_t(0xB00C), uint16_t(0xB00D)}) {
        PinState w = me1Write(addr);
        CHECK(card.respondsToWrite(w, 0x5A));
        PinState r = me1Read(addr);
        CHECK(card.respondsToRead(r, v) && v == 0x5A);
    }

    // OPA read returns input bits only (GetReg(OPA) = r_opa & ~r_dda): the
    // output bits it wrote (incl. the latched-but-ignored RMT bits) read
    // back as 0, and the CE-150 injects no OPA input level.
    { PinState w = me1Write(0xB00C); card.respondsToWrite(w, 0x06); }     // DDA: bits 1,2 (RMT) = output
    { PinState w = me1Write(0xB00E); card.respondsToWrite(w, 0xFF); }     // OPA := 0xFF (only bits 1,2 stick)
    { PinState r = me1Read(0xB00E); card.respondsToRead(r, v); CHECK(v == 0x00); }

    // Unclaimed neighbours: 0xB007 (below the block) is not ours.
    PinState r = me1Read(0xB007);
    CHECK(!card.respondsToRead(r, v));
}

// ── steppers via OPC ──────────────────────────────────────────────────

void writeOpc(Ce150Card& card, uint8_t value) {
    PinState w = me1Write(0xB008);
    card.respondsToWrite(w, value);
}

void test_opc_write_walks_x_and_y_steppers() {
    Ce150Card card;
    // Anchor both motors at ring position 0 (phase 0x9 in both nibbles).
    writeOpc(card, 0x99);
    CHECK(card.mechanism().penX() == 0);
    CHECK(card.mechanism().penY() == 0);

    // Walk X forward 4 ring positions (low nibble), Y still parked (high
    // nibble held at 0x9). 4 half-steps forward = +4 in X's convention.
    int pos = 0;
    for (int i = 0; i < 4; ++i) {
        pos = (pos + 1) % 8;
        writeOpc(card, uint8_t(0x90 | kRing[pos]));
    }
    CHECK(card.mechanism().penX() == 4);
    CHECK(card.mechanism().penY() == 0);

    // Now walk Y forward 3 (high nibble), X parked. Y applies the opposite
    // sign (see AlpsPlotterMechanism::stepXY) -> penY goes negative.
    int ypos = 0;
    for (int i = 0; i < 3; ++i) {
        ypos = (ypos + 1) % 8;
        writeOpc(card, uint8_t((kRing[ypos] << 4) | kRing[pos]));
    }
    CHECK(card.mechanism().penY() == -3);
    CHECK(card.mechanism().penX() == 4); // unchanged: X nibble held constant
}

// ── pen up/down via OPB ───────────────────────────────────────────────

void test_pen_signals_trace_a_stroke() {
    Ce150Card card;
    writeOpc(card, 0x99); // anchor

    { PinState w = me1Write(0xB00D); card.respondsToWrite(w, 0x03); } // DDB: PB0/PB1 output
    CHECK(!card.mechanism().penDown());

    { PinState w = me1Write(0xB00F); card.respondsToWrite(w, 0x02); } // PB1 descending -> pen down
    CHECK(card.mechanism().penDown());
    CHECK(card.mechanism().strokes().size() == 1);

    // move X while down -> stroke extends
    int pos = 0;
    for (int i = 0; i < 3; ++i) { pos = (pos + 1) % 8; writeOpc(card, uint8_t(0x90 | kRing[pos])); }
    CHECK(card.mechanism().strokes().back().points.size() == 4); // pen-down point + 3 moves

    { PinState w = me1Write(0xB00F); card.respondsToWrite(w, 0x01); } // PB0 ascending -> pen up
    CHECK(!card.mechanism().penDown());
}

// ── colour turret at the carriage left stop ───────────────────────────

void test_left_stop_advances_colour_every_third_arrival() {
    Ce150Card card;
    writeOpc(card, 0x99); // anchor X at ring pos 0

    using Color = AlpsPlotterMechanism::PenColor;
    CHECK(card.mechanism().penColor() == Color::Black);

    int pos = 0;
    auto driveLeft = [&](int steps) {
        for (int i = 0; i < steps; ++i) { pos = (pos - 1 + 8) % 8; writeOpc(card, uint8_t(0x90 | kRing[pos])); }
    };
    auto driveRight = [&](int steps) {
        for (int i = 0; i < steps; ++i) { pos = (pos + 1) % 8; writeOpc(card, uint8_t(0x90 | kRing[pos])); }
    };

    // First run to the stop: 1 detent (Rot 0 -> 1), colour unchanged (needs
    // 3 detents). The colour magnet reads true only at Rot == 0 in COLOR0
    // -- i.e. before the first detent or after a full turret revolution --
    // so it is already false once the carriage has clicked once. The stop
    // clamp is at -90 (kCE150LeftStopX), so the leftward runs below have to
    // clear that; each ring-walk step here is one raw penX unit.
    driveLeft(110);
    CHECK(card.mechanism().penX() == -90);      // clamped at the physical stop
    CHECK(card.mechanism().penColor() == Color::Black);
    CHECK(!card.mechanism().colorMagnet());

    // Two more arm/arrive cycles -> the 3rd detent advances the colour.
    // driveRight must clear the arm threshold (-32) so the next leftward
    // pass re-arms; driveLeft must reach the -90 stop.
    driveRight(70); driveLeft(90);
    driveRight(70); driveLeft(90);
    CHECK(card.mechanism().penColor() == Color::Blue);
    CHECK(!card.mechanism().colorMagnet());

    // Holding against the stop must not ratchet further detents.
    Color held = card.mechanism().penColor();
    driveLeft(20);
    CHECK(card.mechanism().penColor() == held);
}

void test_reset_rehomes_but_keeps_ink() {
    Ce150Card card;
    writeOpc(card, 0x99);
    { PinState w = me1Write(0xB00D); card.respondsToWrite(w, 0x03); }
    { PinState w = me1Write(0xB00F); card.respondsToWrite(w, 0x02); } // pen down -> opens a stroke
    int pos = 0;
    for (int i = 0; i < 5; ++i) { pos = (pos + 1) % 8; writeOpc(card, uint8_t(0x90 | kRing[pos])); }
    CHECK(card.mechanism().penX() == 5);
    CHECK(card.mechanism().strokes().size() == 1);

    card.reset();
    CHECK(card.mechanism().penX() == 0);
    CHECK(!card.mechanism().penDown());
    CHECK(card.mechanism().strokes().size() == 1); // ink stays on the paper

    uint8_t v = 0;
    PinState r = me1Read(0xB00D);
    CHECK(card.respondsToRead(r, v) && v == 0x00); // DDB latch cleared
}

// ── integration on a real PC1500Machine over the 60-pin SystemBus ─────

void test_machine_rom_visible_and_me1_block_routed() {
    PC1500Machine machine(PC1500Variant::PC1500A);
    auto rom = fakeRom();
    CHECK(machine.attachCE150(rom.data(), rom.size()));
    CHECK(machine.ce150Attached());

    // ROM byte reachable through resolve()->readOpenBus()->SystemBus.
    CHECK(machine.memory().peek(0xA000) == rom[0]);
    CHECK(machine.memory().peek(0xB123) == rom[0xB123 - 0xA000]);

    // ME1 0xB00x round-trips to the card (DDA latch).
    machine.memory().writeME1(0xB00C, 0x77);
    CHECK(machine.memory().readME1(0xB00C) == 0x77);

    machine.detachCE150();
    CHECK(!machine.ce150Attached());
    CHECK(machine.memory().peek(0xA000) == 0xFF); // open bus again
}

void test_ce150_and_internal_lh5811_coexist() {
    PC1500Machine machine(PC1500Variant::PC1500A);
    auto rom = fakeRom();
    machine.attachCE150(rom.data(), rom.size());

    // The internal LH5811 still answers at its canonical 0xF00x address...
    machine.memory().writeME1(0xF00C, 0xAB); // internal DDA
    CHECK(machine.memory().readME1(0xF00C) == 0xAB);

    // ...independently of the CE-150's own DDA at 0xB00C.
    machine.memory().writeME1(0xB00C, 0x14);
    CHECK(machine.memory().readME1(0xB00C) == 0x14);
    CHECK(machine.memory().readME1(0xF00C) == 0xAB); // internal DDA untouched

    // With no card attached, 0xB00B still reads the internal IF register
    // (0xB00x aliases the internal chip on real hardware).
    PC1500Machine bare(PC1500Variant::PC1500A);
    bare.memory().writeME1(0xF00B, 0x02);
    CHECK((bare.memory().readME1(0xB00B) & 0x02) == 0x02);
}

// ── preset loader: plotter: ce150 ────────────────────────────────────

const char* kSysRom = "roms/PC-1500_A04.ROM";
const char* kCe150Rom = "roms/CE-150.ROM";

bool fileExists(const char* p) { std::ifstream f(p); return f.good(); }

bool writeScratchPreset(const std::string& path, const std::string& body) {
    std::ofstream f(path);
    f << body;
    return f.good();
}

void test_preset_plotter_ce150_attaches_before_reset() {
    if (!fileExists(kSysRom) || !fileExists(kCe150Rom)) {
        std::fprintf(stderr, "SKIP test_preset_plotter_ce150_attaches_before_reset: "
                              "run from the repo root (needs %s + %s)\n", kSysRom, kCe150Rom);
        return;
    }
    const std::string presetPath = "/tmp/calcu1600_ce150_scratch.pc1500a";
    CHECK(writeScratchPreset(presetPath,
        "model: PC-1500A\nfirmware: A04\nplotter: ce150\n"));

    PresetFile preset;
    std::string err;
    CHECK(parsePresetFile(presetPath, &preset, &err));
    CHECK(preset.plotter == "ce150");

    PC1500Machine machine(preset.variant);
    PresetLoadResult res = applyPC1500Preset(machine, preset, {}, ".", ".", {}, {"roms"});
    CHECK(res.ok);
    CHECK(res.ce150Attached);
    CHECK(machine.ce150Attached());

    // The CE-150 ROM in 0x8000-0xBFFF is only visible with PV = 0: the
    // system ROM rests at PV = 1 (its own extension tables) and issues RPV
    // just before jumping into the peripheral ROM (PU-PV-Signals.md). Boot
    // settles at PV = 1, so at rest the window is open bus; drop PV and the
    // CE-150 ROM appears.
    machine.memory().updatePUPV(machine.cpu().pu(), /*pv=*/true);
    CHECK(machine.memory().peek(0xA000) == 0xFF);
    machine.memory().updatePUPV(machine.cpu().pu(), /*pv=*/false);
    CHECK(machine.memory().peek(0xA000) != 0xFF);

    // Same preset, no romDirs -> a clean, specific failure. Main and
    // plotter ROMs share one bundled directory (BundledRomCatalog), so this
    // fails resolving the main ROM itself rather than reaching the plotter
    // step at all -- still a clean failure, not a crash.
    PC1500Machine machine2(preset.variant);
    PresetLoadResult res2 = applyPC1500Preset(machine2, preset);
    CHECK(!res2.ok);
    CHECK(res2.error.find("not found") != std::string::npos);
    CHECK(!machine2.ce150Attached());
}

} // namespace

int run_ce150_tests() {
    test_rom_window_decode_and_negatives();
    test_lh5810_latches_and_direction_masking();
    test_opc_write_walks_x_and_y_steppers();
    test_pen_signals_trace_a_stroke();
    test_left_stop_advances_colour_every_third_arrival();
    test_reset_rehomes_but_keeps_ink();
    test_machine_rom_visible_and_me1_block_routed();
    test_ce150_and_internal_lh5811_coexist();
    test_preset_plotter_ce150_attaches_before_reset();

    std::printf("ce150_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
