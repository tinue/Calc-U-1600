// Headless C++ tests for the CE-158 RS-232C / Centronics interface:
// Ce158Card's ROM window (PV-gated, PU-banked), its LH5811 port model, the
// CDP1854 UART and its pacing, the Centronics capture, and -- with the
// real ROMs -- the whole thing driven by BASIC on a PC1500Machine, alone
// and chained with a CE-150. Same no-framework, assert-and-tally style as
// ce150_tests.cpp.
//
// Build & run: see tools/run_tests.sh

#include <cstdio>
#include <cstdint>
#include <deque>
#include <fstream>
#include <string>
#include <vector>

#include "../Connector/Ce158Card.hpp"
#include "../PC1500/PC1500Machine.hpp"
#include "../PC1500/PC1500PresetLoader.hpp"
#include "../Preset/PresetFile.hpp"
#include "../PC1600/PC1600Machine.hpp"
#include "../PC1600/PC1600PresetLoader.hpp"
#include "../Resources/BundledRomCatalog.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

PinState me1(uint16_t addr, bool forWrite) {
    PinState p; p.address = addr; p.forWrite = forWrite; p.me1 = true; return p;
}
PinState me0Read(uint16_t addr, bool pu, bool pv) {
    PinState p; p.address = addr; p.forWrite = false;
    p.pin[2] = pv; // PV
    p.pin[3] = pu; // PU
    return p;
}

uint8_t rd(Ce158Card& c, uint16_t addr) {
    uint8_t v = 0xEE;
    CHECK(c.respondsToRead(me1(addr, false), v));
    return v;
}
void wr(Ce158Card& c, uint16_t addr, uint8_t v) {
    CHECK(c.respondsToWrite(me1(addr, true), v));
}

std::vector<uint8_t> fakeRom() {
    std::vector<uint8_t> rom(Ce158Card::kRomSize);
    for (size_t i = 0; i < rom.size(); ++i) rom[i] = uint8_t(i * 13 + (i >> 13) * 5 + 1);
    return rom;
}

struct FakeLink : SerialLink {
    std::deque<uint8_t> rx;
    std::vector<uint8_t> tx;
    bool armed = true;          // false: the peer stays silent
    Lines lines;
    int controlCalls = 0;
    bool lastDtr = false, lastRts = false;
    bool poll(uint8_t& out) override {
        if (!armed || rx.empty()) return false;
        out = rx.front();
        rx.pop_front();
        return true;
    }
    void send(uint8_t b) override { tx.push_back(b); }
    void getStatus(Lines& in) override { in = lines; }
    void setControl(bool dtr, bool rts) override { ++controlCalls; lastDtr = dtr; lastRts = rts; }
};

// A card with its ROM loaded and reset, as PC1500Machine::attachCE158 leaves it.
Ce158Card& freshCard(Ce158Card& card) {
    auto rom = fakeRom();
    card.loadRom(rom.data(), rom.size());
    card.reset();
    return card;
}

// ── ROM window ────────────────────────────────────────────────────────

void test_rom_window_pv_gated_and_pu_banked() {
    Ce158Card card;
    auto rom = fakeRom();
    CHECK(!card.loadRom(rom.data(), rom.size() - 1)); // wrong size rejected
    CHECK(card.loadRom(rom.data(), rom.size()));

    uint8_t v = 0;
    CHECK(card.respondsToRead(me0Read(0x8000, /*pu=*/false, /*pv=*/true), v) && v == rom[0]);
    CHECK(card.respondsToRead(me0Read(0x9FFF, false, true), v) && v == rom[0x1FFF]);
    CHECK(card.respondsToRead(me0Read(0x8000, /*pu=*/true, true), v) && v == rom[0x2000]);
    CHECK(card.respondsToRead(me0Read(0x9FFF, true, true), v) && v == rom[0x3FFF]);

    // PV = 0 (the CE-150's half of the peripheral area), outside the window,
    // or a write: not ours.
    CHECK(!card.respondsToRead(me0Read(0x8000, false, /*pv=*/false), v));
    CHECK(!card.respondsToRead(me0Read(0xA000, false, true), v));
    CHECK(!card.respondsToRead(me0Read(0x7FFF, false, true), v));
    PinState w = me0Read(0x8000, false, true); w.forWrite = true;
    CHECK(!card.respondsToWrite(w, 0x55));
}

// ── LH5811 ports ──────────────────────────────────────────────────────

void test_port_a_modem_lines_and_strap() {
    Ce158Card card;
    freshCard(card);
    FakeLink link;
    card.setSerialLink(&link);

    // All inputs after reset: PA2-PA4 asserted (low), low-battery OK (low),
    // PA6/PA7 strap high -- the ROM's CDP1854 decode (see the card header).
    CHECK((rd(card, 0xD00E) & 0xFC) == 0xC0);
    CHECK((rd(card, 0xD00E) & 0x3C) == 0x00); // the ROM's "may send/receive" test

    // Peer drops CTS / DCD / DSR -> PA2 / PA3 / PA4 read high.
    link.lines.cts = false;
    card.tick(1'000'000);
    CHECK((rd(card, 0xD00E) & 0x3C) == 0x04);
    link.lines.cts = true; link.lines.dcd = false; link.lines.dsr = false;
    card.tick(1'000'000);
    CHECK((rd(card, 0xD00E) & 0x3C) == 0x18);

    // Output bits read back the latch; input bits stay live.
    wr(card, 0xD00C, 0xC3);        // DDA: PA0/1/6/7 outputs
    wr(card, 0xD00E, 0x02);        // DTR asserted (PA0 = 0), RTS off, baud bits 0
    CHECK((rd(card, 0xD00E) & 0xC3) == 0x02);
    CHECK(link.lastDtr && !link.lastRts);
    CHECK((rd(card, 0xD00E) & 0x3C) == 0x18); // inputs unchanged

    // PB7 (BUSY) reads 1 = printer ready while PB0-6 are outputs.
    wr(card, 0xD00D, 0x7F);
    wr(card, 0xD00F, 0x2A);
    CHECK(rd(card, 0xD00F) == (0x80 | 0x2A));
}

void test_centronics_strobe_captures_inverted_byte() {
    Ce158Card card;
    freshCard(card);
    wr(card, 0xD00D, 0x7F);
    // The ROM's LPRINT byte path (81BC): PB = ~D >> 1, PC5 = ~D0, pulse PC6.
    auto send = [&](uint8_t d) {
        const uint8_t inv = uint8_t(~d);
        wr(card, 0xD008, uint8_t(0x80 | ((inv & 1) << 5)));   // INIT high, PC5 = ~D0, STROBE low
        wr(card, 0xD00F, uint8_t(inv >> 1));
        wr(card, 0xD008, uint8_t(0xC0 | ((inv & 1) << 5)));   // STROBE rising edge
        wr(card, 0xD008, uint8_t(0x80 | ((inv & 1) << 5)));   // STROBE back low
    };
    send('H'); send('i'); send(0x00); send(0xFF);
    std::vector<uint8_t> out = card.drainParallelOutput();
    CHECK(out.size() == 4);
    if (out.size() == 4) {
        CHECK(out[0] == 'H'); CHECK(out[1] == 'i'); CHECK(out[2] == 0x00); CHECK(out[3] == 0xFF);
    }
    CHECK(card.drainParallelOutput().empty());
    // Only the low-to-high STROBE edge latches: holding it high, or
    // changing other PC bits while it is high, captures nothing more.
    wr(card, 0xD008, 0xC0);
    CHECK(card.drainParallelOutput().size() == 1);
    wr(card, 0xD008, 0xC0);
    wr(card, 0xD008, 0xE0);
    CHECK(card.drainParallelOutput().empty());
}

void test_baud_codes_from_rom_table() {
    Ce158Card card;
    freshCard(card);
    wr(card, 0xD00C, 0xC0);
    // TBL_WORDLEN $70 $58 $56 $4C $48 $44 $42 $41 -> PC = 0x80 | b >> 1,
    // PA7 = b & 1 (HB_CFG_URT_BD at 8B3D).
    const uint8_t table[8] = {0x70, 0x58, 0x56, 0x4C, 0x48, 0x44, 0x42, 0x41};
    const int baud[8] = {50, 100, 110, 200, 300, 600, 1200, 2400};
    for (int i = 0; i < 8; ++i) {
        wr(card, 0xD00E, uint8_t((table[i] & 1) ? 0x80 : 0x00));
        wr(card, 0xD008, uint8_t(0x80 | (table[i] >> 1)));
        CHECK(card.baudRate() == baud[i]);
    }
}

// ── CDP1854 UART ──────────────────────────────────────────────────────

void test_uart_transmit_paced_by_baud() {
    Ce158Card card;
    freshCard(card);
    FakeLink link;
    card.setSerialLink(&link);
    wr(card, 0xD00C, 0xC0);
    wr(card, 0xD008, 0x80 | 0x20); wr(card, 0xD00E, 0x80); // 2400 baud
    CHECK(card.baudRate() == 2400);

    CHECK(rd(card, 0xD203) == (Ce158Card::kStatusTHRE | Ce158Card::kStatusTSRE));
    wr(card, 0xD200, 'Q');
    CHECK((rd(card, 0xD203) & (Ce158Card::kStatusTHRE | Ce158Card::kStatusTSRE)) == 0);
    // One character at 2400 baud = 10 bits = 1.3 MHz * 10 / 2400 = 5416 cycles.
    card.tick(5000);
    CHECK(link.tx.empty());
    card.tick(500);
    CHECK(link.tx.size() == 1 && link.tx[0] == 'Q');
    CHECK((rd(card, 0xD203) & 0xC0) == 0xC0);

    // No link: the byte is still paced and THRE comes back.
    card.setSerialLink(nullptr);
    wr(card, 0xD200, 'Z');
    card.tick(6000);
    CHECK((rd(card, 0xD203) & Ce158Card::kStatusTHRE) != 0);
}

void test_uart_receive_never_overruns() {
    Ce158Card card;
    freshCard(card);
    FakeLink link;
    card.setSerialLink(&link);
    link.rx = {'a', 'b', 'c'};
    const uint64_t charCycles = 1300000 * 10 / 300; // default 300 baud

    card.tick(charCycles);
    CHECK(rd(card, 0xD203) & Ce158Card::kStatusDA);
    // Several character times pass unread: the receiver keeps 'a' and
    // leaves the rest with the peer -- no overrun, nothing lost.
    card.tick(charCycles * 5);
    CHECK(link.rx.size() == 2);
    CHECK((rd(card, 0xD203) & Ce158Card::kStatusOE) == 0);
    CHECK(rd(card, 0xD202) == 'a');
    CHECK((rd(card, 0xD203) & Ce158Card::kStatusDA) == 0);
    card.tick(charCycles);
    CHECK(rd(card, 0xD202) == 'b');
    card.tick(charCycles);
    CHECK(rd(card, 0xD202) == 'c');
    card.tick(charCycles * 3);
    CHECK((rd(card, 0xD203) & Ce158Card::kStatusDA) == 0);

    // A control-register write keeps DA/THRE and latches the byte.
    wr(card, 0xD201, 0x99);
    CHECK(card.uartControl() == 0x99);
    // Interrupt-ID register: claimed, "no interrupt".
    CHECK(rd(card, 0xDE00) == 0x80);
    CHECK(rd(card, 0xDFFF) == 0x80);
}

// ── integration on a real PC1500Machine ───────────────────────────────

void test_machine_decode_alongside_internal_io() {
    PC1500Machine machine(PC1500Variant::PC1500A);
    auto rom = fakeRom();
    CHECK(machine.attachCE158(rom.data(), rom.size()));
    CHECK(machine.ce158Attached());

    machine.memory().updatePUPV(/*pu=*/false, /*pv=*/true);
    CHECK(machine.memory().peek(0x8000) == rom[0]);
    machine.memory().updatePUPV(/*pu=*/true, /*pv=*/true);
    CHECK(machine.memory().peek(0x8000) == rom[0x2000]);
    machine.memory().updatePUPV(false, /*pv=*/false);
    CHECK(machine.memory().peek(0x8000) == 0xFF);

    // ME1 0xD00C reaches the card's DDA; the internal LH5811 (0xF00C) is separate.
    machine.memory().writeME1(0xF00C, 0xAB);
    machine.memory().writeME1(0xD00C, 0x14);
    CHECK(machine.memory().readME1(0xD00C) == 0x14);
    CHECK(machine.memory().readME1(0xF00C) == 0xAB);

    machine.detachCE158();
    CHECK(!machine.ce158Attached());
    machine.memory().updatePUPV(false, true);
    CHECK(machine.memory().peek(0x8000) == 0xFF);
}

const char* kSysRom = "roms/PC-1500_A04.ROM";
const char* kCe150Rom = "roms/CE-150.ROM";
const char* kCe158Rom = "roms/CE-158.ROM";

bool fileExists(const char* p) { std::ifstream f(p); return f.good(); }

bool haveRoms(const char* test) {
    if (fileExists(kSysRom) && fileExists(kCe150Rom) && fileExists(kCe158Rom)) return true;
    std::fprintf(stderr, "SKIP %s: run from the repo root (needs %s, %s, %s)\n", test, kSysRom, kCe150Rom,
                 kCe158Rom);
    return false;
}

bool loadPreset(const std::string& body, PresetFile* preset) {
    const std::string path = "/tmp/calcu1600_ce158_scratch.pc1500a";
    { std::ofstream f(path); f << body; }
    std::string err;
    const bool ok = parsePresetFile(path, preset, &err);
    if (!ok) std::fprintf(stderr, "preset parse error: %s\n", err.c_str());
    return ok;
}

std::string text(const std::vector<uint8_t>& bytes) { return std::string(bytes.begin(), bytes.end()); }

void test_preset_parse_interface_key() {
    PresetFile p;
    CHECK(loadPreset("model: PC-1500A\ninterface: CE-158\n", &p));
    CHECK(p.interfaceName == "ce158");
    PresetFile none;
    CHECK(loadPreset("model: PC-1500\ninterface: none\n", &none));
    CHECK(none.interfaceName.empty());
    PresetFile bad;
    CHECK(!loadPreset("model: PC-1500A\ninterface: ce-999\n", &bad));
}

// LPRINT to the Centronics port with the CE-150 also on the chain: the
// CE-158 prints its line, OPN hands LPRINT back to the CE-150.
void test_rom_lprint_centronics_with_ce150_chained() {
    if (!haveRoms(__func__)) return;
    PresetFile preset;
    CHECK(loadPreset(
        "model: PC-1500A:A04\nplotter: ce150\ninterface: ce158\n"
        "program:\n  format: basic-text\n  text: |\n"
        "    10 OPN \"LPRT\"\n    20 LPRINT \"HELLO CE-158\"\n    30 OPN\n    40 LPRINT \"CE150\"\n"
        "keys:\n  - key: cl\n  - key: mode\n  - type: RUN\n  - wait:\n",
        &preset));
    PC1500Machine machine(preset.variant);
    PresetLoadResult res = applyPC1500Preset(machine, preset, {}, ".", ".", {}, {"roms"});
    CHECK(res.ok);
    CHECK(res.ce150Attached && res.ce158Attached);
    const std::string printed = text(machine.drainCE158ParallelOutput());
    // The parallel port's power-on end code is LF (CONSOLE 80,1).
    CHECK(printed == "HELLO CE-158\n");
    CHECK(!machine.ce150PlotPoints().empty()); // "CE150" went to the plotter
}

// SETDEV PO sends LPRINT out of the serial port; SETDEV KI reads INPUT
// from it. The peer is armed only once INPUT is waiting -- the ROM flushes
// the receiver when SETDEV runs, so an earlier byte would be lost (as on
// the real interface).
void test_rom_serial_lprint_and_input() {
    if (!haveRoms(__func__)) return;
    PresetFile preset;
    CHECK(loadPreset(
        "model: PC-1500A:A04\ninterface: ce158\n"
        "program:\n  format: basic-text\n  text: |\n"
        "    10 SETDEV PO\n    20 OUTSTAT 0\n    30 LPRINT \"SERIAL OUT\"\n"
        "    40 SETDEV KI\n    50 INPUT A$\n    60 SETDEV\n    70 OPN \"LPRT\":LPRINT \"GOT \";A$\n"
        "keys:\n  - key: cl\n  - key: mode\n  - type: RUN\n",
        &preset));
    PC1500Machine machine(preset.variant);
    FakeLink link;
    link.armed = false;
    link.rx = {'A', 'B', 'C', '1', '2', '3', '\r'};
    machine.setCE158SerialLink(&link);
    PresetLoadResult res = applyPC1500Preset(machine, preset, {}, ".", ".", {}, {"roms"});
    CHECK(res.ok);
    machine.runCycles(1300000); // let INPUT start waiting
    CHECK(text(link.tx) == "SERIAL OUT\r");
    link.armed = true;
    machine.runCycles(1300000 * 2);
    CHECK(link.rx.empty());
    const std::string got = text(machine.drainCE158ParallelOutput());
    CHECK(got == "GOT ABC123\n");
    machine.setCE158SerialLink(nullptr);
}

// ── PC-1600: the CE-158 on the LH5803 side ─────────────────────────────

void test_pc1600_lh5803_window_and_io_routing() {
    PC1600Machine m;
    auto rom = fakeRom();
    std::vector<uint8_t> ce150(Ce150Card::kRomSize, 0x5A);
    CHECK(m.attachCE158(rom.data(), rom.size()));
    CHECK(m.attachCE150(ce150.data(), ce150.size())); // coexist on the bus
    CHECK(m.ce158Attached() && m.ce150Attached());

    auto& mem = m.lh5803Memory();
    mem.updatePUPV(/*pu=*/false, /*pv=*/true);
    CHECK(mem.readME0(0x8000) == rom[0]);
    CHECK(mem.readME0(0xA000) == 0xFF);            // CE-150 half is PV = 0
    mem.updatePUPV(/*pu=*/true, /*pv=*/true);
    CHECK(mem.readME0(0x8000) == rom[0x2000]);     // PU picks the high bank
    mem.updatePUPV(false, /*pv=*/false);
    CHECK(mem.readME0(0x8000) == 0xFF);
    CHECK(mem.readME0(0xA000) == 0x5A);

    // ME1 register blocks reach the card, not the LH5803 ROM underneath.
    mem.writeME1(0xD00C, 0x3C);
    CHECK(mem.readME1(0xD00C) == 0x3C);
    CHECK(mem.readME1(0xD203) == (Ce158Card::kStatusTHRE | Ce158Card::kStatusTSRE));
    CHECK(mem.readME1(0xDE00) == 0x80);

    // ME1 8000-BFFF is an I/O cycle: never a card ROM byte, whatever PV is.
    mem.updatePUPV(false, /*pv=*/true);
    CHECK(mem.readME1(0x8000) == 0xFF);
    mem.updatePUPV(false, /*pv=*/false);
    CHECK(mem.readME1(0xA000) == 0xFF);
    CHECK(mem.readME1(0xB010) == 0xFF);
    CHECK(mem.readME1(0xB000) == 0xFF); // B000-B007: LH5810 select, no register

    m.detachCE158();
    CHECK(mem.readME1(0xD00C) != 0x3C || mem.readME0(0xD00C) == 0x3C); // falls back to the ROM alias
    mem.updatePUPV(false, true);
    CHECK(mem.readME0(0x8000) == 0xFF);
}

void test_pc1600_ce158_and_ce1600p_exclusive() {
    PC1600Machine m;
    auto rom = fakeRom();
    std::vector<uint8_t> half(CE1600PCard::kRomHalfSize, 0x11);
    CHECK(m.attachCE158(rom.data(), rom.size()));
    CHECK(m.attachCE1600P(half.data(), half.size(), half.data(), half.size()));
    CHECK(m.ce1600pAttached() && !m.ce158Attached());
    CHECK(m.attachCE158(rom.data(), rom.size()));
    CHECK(m.ce158Attached() && !m.ce1600pAttached());

    PresetFile p;
    CHECK(!loadPreset("model: PC-1600\nplotter: ce1600p\ninterface: ce158\n", &p));
    PresetFile ok;
    CHECK(loadPreset("model: PC-1600\nplotter: ce150\ninterface: ce158\n", &ok));
    CHECK(ok.interfaceName == "ce158" && ok.plotter == "ce150");
}

bool havePC1600Roms(const char* test) {
    if (fileExists("roms/PC1600-P0-B0-new.bin") && fileExists(kCe158Rom)) return true;
    std::fprintf(stderr, "SKIP %s: run from the repo root (needs the PC-1600 ROM set + %s)\n", test, kCe158Rom);
    return false;
}

std::string pc1600Preset(const std::string& program) {
    return "model: PC-1600\ninterface: ce158\n"
           "keys:\n  - key: mode\n  - type: NEW\n  - type: MODE1\n"
           "program:\n  format: basic-text\n  text: |\n" + program +
           "keys:\n  - key: mode\n  - type: RUN\n";
}

// MODE 1: LPRINT through the CE-158's parallel port, then (SETDEV PO) its
// serial port, from PC-1600 BASIC via the LH5803.
void test_pc1600_rom_mode1_printing() {
    if (!havePC1600Roms(__func__)) return;
    PresetFile preset;
    CHECK(loadPreset(pc1600Preset("    10 OPN \"LPRT\"\n    20 LPRINT \"HELLO 1600\"\n    30 OPN\n"
                                  "    40 SETDEV PO\n    50 OUTSTAT 0\n    60 LPRINT \"SERIAL 1600\"\n") +
                         "  - wait:\n",
                     &preset));
    PC1600Machine machine;
    std::string romErr;
    CHECK(BundledRoms::loadPC1600RomSet(machine, {"roms"}, "new", &romErr));
    FakeLink link;
    machine.setCE158SerialLink(&link);
    PC1600PresetLoadResult res = applyPC1600Preset(machine, preset, {}, ".", ".", {}, {"roms"});
    if (!res.ok) std::fprintf(stderr, "preset error: %s\n", res.error.c_str());
    CHECK(res.ok);
    CHECK(res.ce158Attached);
    CHECK(text(machine.drainCE158ParallelOutput()) == "HELLO 1600\n");
    CHECK(text(link.tx) == "SERIAL 1600\r");
    machine.setCE158SerialLink(nullptr);
}

// MODE 1: RINKEY$ reads the CE-158's UART. The peer is armed once the
// program is polling.
void test_pc1600_rom_mode1_rinkey() {
    if (!havePC1600Roms(__func__)) return;
    PresetFile preset;
    CHECK(loadPreset(pc1600Preset("    10 B$=RINKEY$\n    20 IF LEN(B$)=0 THEN 10\n"
                                  "    30 OPN \"LPRT\":LPRINT ASC(B$)\n"),
                     &preset));
    PC1600Machine machine;
    std::string romErr;
    CHECK(BundledRoms::loadPC1600RomSet(machine, {"roms"}, "new", &romErr));
    FakeLink link;
    link.armed = false;
    link.rx = {'A'};
    machine.setCE158SerialLink(&link);
    PC1600PresetLoadResult res = applyPC1600Preset(machine, preset, {}, ".", ".", {}, {"roms"});
    if (!res.ok) std::fprintf(stderr, "preset error: %s\n", res.error.c_str());
    CHECK(res.ok);
    machine.runCycles(PC1600Machine::kTStateHz);
    link.armed = true;
    machine.runCycles(PC1600Machine::kTStateHz * 3);
    CHECK(link.rx.empty());
    CHECK(text(machine.drainCE158ParallelOutput()) == " 65\n");
    machine.setCE158SerialLink(nullptr);
}

} // namespace

int run_ce158_tests() {
    test_rom_window_pv_gated_and_pu_banked();
    test_port_a_modem_lines_and_strap();
    test_centronics_strobe_captures_inverted_byte();
    test_baud_codes_from_rom_table();
    test_uart_transmit_paced_by_baud();
    test_uart_receive_never_overruns();
    test_machine_decode_alongside_internal_io();
    test_preset_parse_interface_key();
    test_rom_lprint_centronics_with_ce150_chained();
    test_rom_serial_lprint_and_input();
    test_pc1600_lh5803_window_and_io_routing();
    test_pc1600_ce158_and_ce1600p_exclusive();
    test_pc1600_rom_mode1_printing();
    test_pc1600_rom_mode1_rinkey();

    std::printf("ce158_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
