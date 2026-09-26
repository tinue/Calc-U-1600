// Headless end-to-end test for the preset loader's `- trace:` step
// (PC1500PresetLoader.cpp) -- a port of Calc-U-59's `KEYSTROKES:` `Trace:`
// directive. Drives applyPC1500Preset() against a real ROM
// (roms/PC-1500_A04.ROM, relative to the repo root) with a preset that
// starts a CPU instruction trace, runs a bit, and stops it, then parses
// the produced file back with a minimal inline reader and checks it is a
// well-formed CALCU1500_TRACE stream (magic/version, SESSION_START first,
// >=1 TRACE_EVENT, SESSION_END last). Skips (not fails) if the ROM is
// missing, same convention as basictyper_tests.cpp / lh5801_tests.cpp's
// test_boot_smoke_real_rom.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>   // mkdtemp

#include "../PC1500/PC1500Machine.hpp"
#include "../Preset/PresetFile.hpp"
#include "../PC1500/PC1500PresetLoader.hpp"
#include "../PC1500/PC1500TraceFile.hpp"
#include "../PC1600/PC1600Machine.hpp"
#include "../PC1600/PC1600PresetLoader.hpp"
#include "PresetTestSupport.hpp"
#include "TestRoms.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

const char* kRomPath = "roms/PC-1500_A04.ROM";

bool romPresent() {
    std::ifstream f(kRomPath, std::ios::binary);
    return static_cast<bool>(f);
}

// ── Minimal CALCU1500_TRACE reader ─────────────────────────────────────
// Constants MUST match Core/PC1500/PC1500TraceFile.cpp.
struct TraceSummary {
    bool headerOk = false;
    int firstRecordType = -1;
    int lastRecordType = -1;
    int sessionStartCount = 0;
    int traceEventCount = 0;
    int sessionEndCount = 0;
    int gapCount = 0;
    int z80EventCount = 0;          // 0x04 TRACE_EVENT_Z80 (the PC-1600's SC7852)
    bool truncated = false;
    bool allEventPayloads25 = true; // v2: 25-byte TRACE_EVENT (added cpuId)
    bool allZ80Payloads29 = true;   // 29-byte TRACE_EVENT_Z80
};

uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t le16(const uint8_t* p) { return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8)); }

TraceSummary readTrace(const std::string& path) {
    TraceSummary s;
    std::ifstream in(path, std::ios::binary);
    if (!in) return s;
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    if (buf.size() < 16) { s.truncated = true; return s; }
    s.headerOk = (le32(buf.data()) == 0x50433135u) && (le16(buf.data() + 4) == 2);

    size_t i = 16;
    while (i + 3 <= buf.size()) {
        uint8_t type = buf[i];
        uint16_t len = le16(buf.data() + i + 1);
        if (i + 3 + len > buf.size()) { s.truncated = true; break; }
        if (s.firstRecordType < 0) s.firstRecordType = type;
        s.lastRecordType = type;
        switch (type) {
            case 0x01: s.sessionStartCount++; break;
            case 0x02: s.traceEventCount++; if (len != 25) s.allEventPayloads25 = false; break;
            case 0x03: s.sessionEndCount++; break;
            case 0x04: s.z80EventCount++; if (len != 29) s.allZ80Payloads29 = false; break;
            case 0x05: s.gapCount++; break;
            default: break;
        }
        i += 3 + len;
    }
    return s;
}

std::string makeTempDir() {
    char tmpl[] = "/tmp/calcu1600_trace_XXXXXX";
    char* dir = mkdtemp(tmpl);
    return dir ? std::string(dir) : std::string();
}

// A preset that explicitly starts and stops a trace produces a complete
// stream: SESSION_START ... TRACE_EVENT(s) ... SESSION_END.
void test_trace_step_produces_wellformed_file() {
    if (!romPresent()) {
        std::fprintf(stderr, "SKIP test_trace_step_produces_wellformed_file: %s not found "
                              "relative to cwd (run tests from the repo root)\n", kRomPath);
        return;
    }
    std::string dir = makeTempDir();
    CHECK(!dir.empty());
    if (dir.empty()) return;

    PresetFile preset;
    std::string err;
    CHECK(parsePresetString(
        "model: PC-1500A\n"
        "keys:\n"
        "  - trace: t.bin\n"
        "  - wait: 0.3\n"
        "  - trace: off\n",
        dir + "/scratch.pc1500", &preset, &err));

    PC1500Machine machine(preset.variant);
    PresetLoadResult res = applyPC1500Preset(machine, preset, {}, dir, ".", {}, {"roms"});
    CHECK(res.ok);

    TraceSummary s = readTrace(dir + "/t.bin");
    CHECK(s.headerOk);
    CHECK(s.firstRecordType == 0x01);   // SESSION_START
    CHECK(s.lastRecordType == 0x03);    // SESSION_END
    CHECK(s.sessionStartCount == 1);
    CHECK(s.sessionEndCount == 1);
    CHECK(s.traceEventCount > 0);
    CHECK(s.allEventPayloads25);
    CHECK(!s.truncated);

    std::remove((dir + "/t.bin").c_str());
    std::remove((dir + "/scratch.pc1500").c_str());
    std::remove(dir.c_str());
}

// A trace left running at the end of the preset is still finalised
// (SESSION_END written, file closed) by runPresetSections()' TraceCloser guard
// -- mirrors Calc-U-59's auto-close of a scripted trace.
void test_trace_left_open_is_auto_closed() {
    if (!romPresent()) {
        std::fprintf(stderr, "SKIP test_trace_left_open_is_auto_closed: %s not found\n", kRomPath);
        return;
    }
    std::string dir = makeTempDir();
    CHECK(!dir.empty());
    if (dir.empty()) return;

    PresetFile preset;
    std::string err;
    CHECK(parsePresetString(
        "model: PC-1500A\n"
        "keys:\n"
        "  - trace: t2.bin\n"
        "  - wait: 0.2\n",
        dir + "/scratch.pc1500", &preset, &err));

    PC1500Machine machine(preset.variant);
    PresetLoadResult res = applyPC1500Preset(machine, preset, {}, dir, ".", {}, {"roms"});
    CHECK(res.ok);

    TraceSummary s = readTrace(dir + "/t2.bin");
    CHECK(s.headerOk);
    CHECK(s.firstRecordType == 0x01);
    CHECK(s.lastRecordType == 0x03);   // finalised despite no `- trace: off`
    CHECK(s.traceEventCount > 0);

    std::remove((dir + "/t2.bin").c_str());
    std::remove((dir + "/scratch.pc1500").c_str());
    std::remove(dir.c_str());
}

// PC1600Machine's trace draining writes both frame shapes into the SAME
// TRACE.bin (Calc-U-1600's PC-1600 dual-CPU trace support) -- exercised
// here directly via PC1500TraceFile rather than a full preset/GUI round
// trip (no `.pc1600` preset format exists yet). Confirms the
// TRACE_EVENT_Z80 (0x04) record's 29-byte payload survives a write/read
// round trip alongside an ordinary (LH5801-shaped) TRACE_EVENT in the
// same file, both readable by a single linear scan.
void test_z80_frame_shares_one_file_with_lh5801_frame() {
    std::string dir = makeTempDir();
    CHECK(!dir.empty());
    if (dir.empty()) return;
    std::string path = dir + "/z80.bin";

    uint64_t counted = 0;
    {
        std::FILE* fh = std::fopen(path.c_str(), "wb");
        CHECK(fh != nullptr);
        PC1500TraceFile tf(fh); // writes header + SESSION_START

        CpuFrame lh{};
        lh.seqno = 1; lh.pc = 0x1234; lh.cpuId = CPU_ID_LH5803;
        tf.writeFrame(lh);

        Z80CpuFrame z{};
        z.seqno = 2; z.pc = 0x5678; z.cpuId = CPU_ID_SC7852; z.af = 0xAABB;
        tf.writeFrame(z);

        tf.finish(); // writes SESSION_END, closes
        counted = tf.bytesWritten();
    }

    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(buf.size() >= 16);
    CHECK(counted == buf.size()); // what the GUI's size cap reads instead of stat()
    CHECK(le32(buf.data()) == 0x50433135u);
    CHECK(le16(buf.data() + 4) == 2); // version

    size_t i = 16;
    int lh5801Events = 0, z80Events = 0, sessionEnds = 0;
    while (i + 3 <= buf.size()) {
        uint8_t type = buf[i];
        uint16_t len = le16(buf.data() + i + 1);
        CHECK(i + 3 + len <= buf.size());
        if (i + 3 + len > buf.size()) break;
        if (type == 0x02) { CHECK(len == 25); lh5801Events++; }
        if (type == 0x04) { CHECK(len == 29); z80Events++; }
        if (type == 0x03) sessionEnds++;
        i += 3 + len;
    }
    CHECK(lh5801Events == 1);
    CHECK(z80Events == 1);
    CHECK(sessionEnds == 1);

    std::remove(path.c_str());
    std::remove(dir.c_str());
}

// ── PC-1600 `- trace:` step, end to end ────────────────────────────────
// A PC-1600 preset that starts and stops a trace produces a complete
// stream, and -- because both CPUs run during boot -- it carries BOTH a
// 25-byte LH5803-shaped TRACE_EVENT (0x02) and a 29-byte SC7852-shaped
// TRACE_EVENT_Z80 (0x04) in the one file.
void test_pc1600_trace_step_produces_wellformed_file() {
    PC1600Machine machine;
    if (!loadPC1600Roms(machine)) {
        std::fprintf(stderr, "SKIP test_pc1600_trace_step_produces_wellformed_file: PC-1600 ROM images not found\n");
        return;
    }
    std::string dir = makeTempDir();
    CHECK(!dir.empty());
    if (dir.empty()) return;

    PresetFile preset;
    std::string err;
    // A `type:` step inside the trace window drives the ROM's line editor,
    // which runs on both CPUs -- so the file gets both frame shapes.
    CHECK(parsePresetString(
        "model: PC-1600\n"
        "keys:\n"
        "  - trace: t.bin\n"
        "  - type: ABC\n"
        "  - wait: 0.3\n"
        "  - trace: off\n",
        dir + "/scratch.pc1600", &preset, &err));

    PresetLoadResult res = applyPC1600Preset(machine, preset, {}, dir);
    CHECK(res.ok);

    TraceSummary s = readTrace(dir + "/t.bin");
    CHECK(s.headerOk);
    CHECK(s.firstRecordType == 0x01);   // SESSION_START
    CHECK(s.lastRecordType == 0x03);    // SESSION_END
    CHECK(s.sessionStartCount == 1);
    CHECK(s.sessionEndCount == 1);
    CHECK(s.traceEventCount > 0);       // LH5803 frames
    CHECK(s.z80EventCount > 0);         // SC7852 frames
    CHECK(s.allEventPayloads25);
    CHECK(s.allZ80Payloads29);
    CHECK(!s.truncated);

    std::remove((dir + "/t.bin").c_str());
    std::remove((dir + "/scratch.pc1600").c_str());
    std::remove(dir.c_str());
}

// A PC-1600 trace left running at the end of the preset is still finalised
// by applyPC1600Preset()'s TraceCloser guard.
void test_pc1600_trace_left_open_is_auto_closed() {
    PC1600Machine machine;
    if (!loadPC1600Roms(machine)) {
        std::fprintf(stderr, "SKIP test_pc1600_trace_left_open_is_auto_closed: PC-1600 ROM images not found\n");
        return;
    }
    std::string dir = makeTempDir();
    CHECK(!dir.empty());
    if (dir.empty()) return;

    PresetFile preset;
    std::string err;
    CHECK(parsePresetString(
        "model: PC-1600\n"
        "keys:\n"
        "  - trace: t2.bin\n"
        "  - type: ABC\n"
        "  - wait: 0.2\n",
        dir + "/scratch.pc1600", &preset, &err));

    PresetLoadResult res = applyPC1600Preset(machine, preset, {}, dir);
    CHECK(res.ok);

    TraceSummary s = readTrace(dir + "/t2.bin");
    CHECK(s.headerOk);
    CHECK(s.firstRecordType == 0x01);
    CHECK(s.lastRecordType == 0x03);   // finalised despite no `- trace: off`
    CHECK(s.traceEventCount > 0);

    std::remove((dir + "/t2.bin").c_str());
    std::remove((dir + "/scratch.pc1600").c_str());
    std::remove(dir.c_str());
}

// Not a trace test, but the same ROM-driven applyPC1500Preset() setup: a
// `format: binary` block that isn't all RAM, or runs past &FFFF, fails the
// preset instead of dropping or wrapping bytes and reporting success.
void test_binary_program_outside_ram_fails() {
    if (!romPresent()) {
        std::fprintf(stderr, "SKIP test_binary_program_outside_ram_fails: %s not found\n", kRomPath);
        return;
    }
    std::string dir = makeTempDir();
    CHECK(!dir.empty());
    if (dir.empty()) return;
    {
        std::ofstream bin(dir + "/code.bin", std::ios::binary);
        bin.write("\x01\x02\x03\x04", 4);
    }
    auto run = [&](const char* address) {
        PresetFile preset;
        std::string err;
        CHECK(parsePresetString(std::string("model: PC-1500A\nprogram:\n  format: binary\n  path: code.bin\n"
                                            "  address: ") + address + "\n",
                                dir + "/scratch.pc1500a", &preset, &err));
        PC1500Machine machine(preset.variant);
        return applyPC1500Preset(machine, preset, {}, dir, ".", {}, {"roms"});
    };
    PresetLoadResult rom = run("0xC000");
    CHECK(!rom.ok && rom.error.find("not RAM") != std::string::npos);
    PresetLoadResult wrap = run("0xFFFE");
    CHECK(!wrap.ok && wrap.error.find("past &FFFF") != std::string::npos);
    CHECK(run("0x7C01").ok);

    std::remove((dir + "/code.bin").c_str());
    std::remove(dir.c_str());
}

// `format: binary` with a CE-158 header on a PC-1500: the payload (not the
// 27 header bytes) lands at the header's load address, with no `address:`.
// A headerless file without `address:` is refused at load time.
void test_binary_program_ce158_header() {
    if (!romPresent()) {
        std::fprintf(stderr, "SKIP test_binary_program_ce158_header: %s not found\n", kRomPath);
        return;
    }
    std::string dir = makeTempDir();
    CHECK(!dir.empty());
    if (dir.empty()) return;
    {
        // CE-158 header: 01 'B' "COM", 16-byte name, then big-endian load
        // address, length - 1 and auto-run address (0 = none).
        std::vector<char> header = {0x01, 0x42, 'C', 'O', 'M'};
        header.resize(5 + 16, 0);
        for (uint16_t v : {uint16_t{0x7C10}, uint16_t{4 - 1}, uint16_t{0}}) {
            header.push_back(static_cast<char>(v >> 8));
            header.push_back(static_cast<char>(v & 0xFF));
        }
        std::ofstream bin(dir + "/ce158.bin", std::ios::binary);
        bin.write(header.data(), static_cast<std::streamsize>(header.size()));
        bin.write("\x11\x22\x33\x44", 4);
        std::ofstream raw(dir + "/raw.bin", std::ios::binary);
        raw.write("\x11\x22", 2);
    }
    auto run = [&](const char* file) {
        PresetFile preset;
        std::string err;
        CHECK(parsePresetString(std::string("model: PC-1500A\nprogram:\n  format: binary\n  path: ") + file + "\n",
                                dir + "/scratch.pc1500a", &preset, &err));
        PC1500Machine machine(preset.variant);
        PresetLoadResult res = applyPC1500Preset(machine, preset, {}, dir, ".", {}, {"roms"});
        return std::make_pair(res, std::vector<uint8_t>{machine.debugPeek(0x7C10), machine.debugPeek(0x7C13)});
    };
    auto [loaded, bytes] = run("ce158.bin");
    CHECK(loaded.ok);
    CHECK(bytes[0] == 0x11 && bytes[1] == 0x44);
    auto [headerless, unused] = run("raw.bin");
    (void)unused;
    CHECK(!headerless.ok && headerless.error.find("'address' is required") != std::string::npos);
    std::remove((dir + "/ce158.bin").c_str());
    std::remove((dir + "/raw.bin").c_str());
    std::remove(dir.c_str());
}

} // namespace

int run_presetloader_trace_tests() {
    test_trace_step_produces_wellformed_file();
    test_trace_left_open_is_auto_closed();
    test_z80_frame_shares_one_file_with_lh5801_frame();
    test_pc1600_trace_step_produces_wellformed_file();
    test_pc1600_trace_left_open_is_auto_closed();
    test_binary_program_outside_ram_fails();
    test_binary_program_ce158_header();

    std::printf("presetloader_trace_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
