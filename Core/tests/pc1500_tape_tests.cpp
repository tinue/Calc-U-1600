// Cassette CLOAD / CSAVE on the PC-1500A through the CE-150, end to end
// against the real ROMs: the CE-150 tape code drives the LH5811's serial
// transmitter and F-register modulation (SDO) and reads PB2 against the
// CL1 timer; the recorder is the machine's TapeDeck. SKIPs (not fails)
// when the ROM images are absent.
//
// The fixture in fixtures/tape/ was made with Pocket Tools 2.1.1:
//   bas2img -p 1500 pc1500_tape.bas pc1500_tape.img
//   bin2wav -p 1500 -l 3 -s 3 -nTAPEFIX pc1500_tape.img pc1500_tape.wav
// (-s 3: a 3 s leader -- the ROM needs more than bin2wav's 0.5 s default
// once the remote has started the motor. The ROM's own CSAVE writes ~8 s.)
//
// Build & run: see tools/run_tests.sh

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../PC1500/PC1500BasicTyper.hpp"
#include "../PC1500/PC1500LcdText.hpp"
#include "../PC1500/PC1500Machine.hpp"
#include "TestRoms.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

constexpr uint64_t kSecond = PC1500Machine::kCpuHz;
const char* kFixtureWav = "Core/tests/fixtures/tape/pc1500_tape.wav";
const char* kFixtureImg = "Core/tests/fixtures/tape/pc1500_tape.img";

std::vector<uint8_t> readFile(const char* path) {
    std::vector<uint8_t> bytes;
    CHECK(readRomImage(path, &bytes));
    return bytes;
}

// A04 ROM + CE-150, cold boot, NEW0.
bool bootWithCE150(PC1500Machine& m) {
    std::vector<uint8_t> ce150;
    if (!m.loadROMFile("roms/PC-1500_A04.ROM") || !readRomImage("roms/CE-150.ROM", &ce150)) return false;
    if (!m.attachCE150(ce150.data(), ce150.size())) return false;
    m.reset();
    m.runCycles(2 * kSecond);
    waitIdle(m, 5 * kSecond);
    for (int i = 0; i < 2; ++i) { // "NEW0? :CHECK" -- see piezo_sampler_tests.cpp
        tapKey(m, "cl");
        m.runCycles(kSecond / 2);
    }
    std::string err;
    typeLine(m, "NEW0", /*pressEnter=*/true, &err);
    waitUntilBasicIdle(m, 10 * kSecond);
    return true;
}

// Types `line` + ENTER and runs until the BASIC prompt is back.
void type(PC1500Machine& m, const std::string& line) {
    std::string err;
    CHECK(typeLine(m, line, /*pressEnter=*/true, &err));
    waitUntilBasicIdle(m, 300 * kSecond);
    waitIdle(m, kSecond);
}

std::string screen(PC1500Machine& m) { return pc1500LcdText(m).plainText(); }

// The program area, from BASPRG_ST (7865H, big-endian).
bool programIs(PC1500Machine& m, const std::vector<uint8_t>& image) {
    const uint16_t start = static_cast<uint16_t>(m.debugPeek(0x7865) << 8 | m.debugPeek(0x7866));
    for (size_t i = 0; i < image.size(); ++i)
        if (m.debugPeek(static_cast<uint16_t>(start + i)) != image[i]) return false;
    return true;
}

std::vector<float> asPlayback(const std::vector<int16_t>& recording) {
    std::vector<float> out;
    out.reserve(recording.size());
    for (int16_t v : recording) out.push_back(v / 32768.0f);
    return out;
}

// A bin2wav tape CLOADs into exactly bas2img's program image.
void test_cload_bin2wav_tape() {
    const std::vector<uint8_t> image = readFile(kFixtureImg);
    PC1500Machine m(PC1500Variant::PC1500A);
    if (image.empty() || !bootWithCE150(m)) {
        std::fprintf(stderr, "SKIP test_cload_bin2wav_tape (PC-1500): ROM images or fixture not found\n");
        return;
    }
    std::string error;
    CHECK(m.tapePlay(kFixtureWav, error));
    CHECK(!m.tapeStatus().motor);
    const uint64_t buzzerEdges = m.memory().piezo().edgeCount();
    type(m, "CLOAD");
    CHECK(screen(m).find("ERROR") == std::string::npos);
    // The tape sounds on the buzzer through its gate (CMT IN), as on a
    // real unit.
    CHECK(m.memory().piezo().edgeCount() - buzzerEdges > 10000);
    CHECK(programIs(m, image));
    CHECK(!m.tapeStatus().motor); // REMOTE off again
    CHECK(m.tapeStatus().position > 10.0);
}

// CSAVE onto a blank tape, then CLOAD that recording into a fresh machine.
void test_csave_cload_round_trip() {
    const std::vector<uint8_t> image = readFile(kFixtureImg);
    PC1500Machine a(PC1500Variant::PC1500A);
    if (image.empty() || !bootWithCE150(a)) {
        std::fprintf(stderr, "SKIP test_csave_cload_round_trip (PC-1500): ROM images or fixture not found\n");
        return;
    }
    std::string error;
    CHECK(a.tapePlay(kFixtureWav, error));
    type(a, "CLOAD");
    a.tapeRecord("");
    const uint64_t buzzerEdges = a.memory().piezo().edgeCount();
    type(a, "CSAVE \"ROUNDTRIP\"");
    CHECK(a.memory().piezo().edgeCount() - buzzerEdges > 10000); // SDO through the buzzer gate
    CHECK(screen(a).find("ERROR") == std::string::npos);
    const std::vector<int16_t> recording = a.tapeDeck().recording();
    CHECK(recording.size() > 10 * TapeDeck::kRecordSampleRate);

    PC1500Machine b(PC1500Variant::PC1500A);
    CHECK(bootWithCE150(b));
    b.tapeDeck().loadForPlay(asPlayback(recording), TapeDeck::kRecordSampleRate);
    type(b, "CLOAD \"ROUNDTRIP\"");
    CHECK(screen(b).find("ERROR") == std::string::npos);
    CHECK(programIs(b, image));

    // CLOAD? compares the tape with memory; a dropout in the data makes it
    // fail. The recording starts with ~8 s of leader tone and its data ends
    // where the signal does, so 3 s before that end is inside the data.
    b.tapeDeck().loadForPlay(asPlayback(recording), TapeDeck::kRecordSampleRate);
    type(b, "CLOAD? \"ROUNDTRIP\"");
    CHECK(screen(b).find("ERROR") == std::string::npos);

    std::vector<int16_t> damaged = recording;
    size_t end = damaged.size();
    while (end > 0 && std::abs(damaged[end - 1]) < 1000) --end;
    const size_t rate = TapeDeck::kRecordSampleRate;
    CHECK(end > 4 * rate);
    for (size_t i = end - 3 * rate; i < end - 3 * rate + rate / 5; ++i) damaged[i] = 0;
    b.tapeDeck().loadForPlay(asPlayback(damaged), TapeDeck::kRecordSampleRate);
    type(b, "CLOAD? \"ROUNDTRIP\"");
    CHECK(screen(b).find("ERROR") != std::string::npos);
}

} // namespace

int run_pc1500_tape_tests() {
    test_cload_bin2wav_tape();
    test_csave_cload_round_trip();
    std::printf("pc1500_tape_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
