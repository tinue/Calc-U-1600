// Cassette CLOAD / CSAVE on the PC-1600 (MODE 0) through the CE-1600P's
// cassette interface, end to end against the real ROMs: the bank-5 tape
// driver bit-bangs OPC 18H b7 and times PB2 itself; the recorder is the
// machine's TapeDeck. SKIPs (not fails) when the ROM images are absent.
//
// The fixture in fixtures/tape/ was made with Pocket Tools 2.1.1:
//   bas2img -p 1600 pc1600_tape.bas pc1600_tape.img
//   bin2wav -p 1600 -l 3 -s 3 -nTAPEFIX pc1600_tape.img pc1600_tape.wav
// (-s 3: a 3 s leader. bin2wav's default 0.5 s one is too short once the
// ROM's ~0.6 s motor start-up delay has eaten into it -- CMSYNC needs 5000
// leader cycles after that, the ROM's own CSAVE writes 10000.)
//
// Build & run: see tools/run_tests.sh

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../PC1600/PC1600LcdText.hpp"
#include "TestRoms.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

constexpr uint64_t kSecond = PC1600Machine::kTStateHz;
constexpr uint16_t kProgramStart = 0xC0C5; // S0 BASIC program, Z-80 view
const char* kFixtureWav = "Core/tests/fixtures/tape/pc1600_tape.wav";
const char* kFixtureImg = "Core/tests/fixtures/tape/pc1600_tape.img";

std::vector<uint8_t> readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool bootWithCE1600P(PC1600Machine& m) {
    std::string error;
    if (!loadPC1600Roms(m) || !BundledRoms::attachCE1600P(m, {"roms"}, "new", &error)) return false;
    m.allReset();
    runBootToPrompt(m);
    tapKey(m, "mode"); // RUN -> PRO
    waitIdle(m, kSecond);
    return true;
}

// Types `line` + ENTER and runs until the BASIC prompt is back (a tape
// command takes tens of emulated seconds).
void type(PC1600Machine& m, const std::string& line) {
    std::string err;
    CHECK(typeLine(m, line, /*pressEnter=*/true, &err));
    waitUntilBasicIdle(m, 300 * kSecond);
    waitIdle(m, kSecond);
}

std::string screen(PC1600Machine& m) {
    std::string all;
    for (const std::string& row : pc1600LcdText(m).plainRows) all += row + "\n";
    return all;
}

bool programIs(PC1600Machine& m, const std::vector<uint8_t>& image) {
    for (size_t i = 0; i < image.size(); ++i)
        if (m.debugPeek(static_cast<uint16_t>(kProgramStart + i)) != image[i]) return false;
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
    PC1600Machine m;
    if (image.empty() || !bootWithCE1600P(m)) {
        std::fprintf(stderr, "SKIP test_cload_bin2wav_tape: PC-1600 ROM images or fixture not found\n");
        return;
    }
    type(m, "NEW0");
    std::string error;
    CHECK(m.tapePlay(kFixtureWav, error));
    CHECK(!m.tapeStatus().motor); // nothing runs the tape before CLOAD
    type(m, "CLOAD");
    const std::string lcd = screen(m);
    CHECK(lcd.find("TAPEFIX") != std::string::npos);
    CHECK(lcd.find("ERROR") == std::string::npos);
    CHECK(programIs(m, image));
    CHECK(!m.tapeStatus().motor); // the ROM stops the motor again
    CHECK(m.tapeStatus().position > 5.0);
    if (lcd.find("TAPEFIX") == std::string::npos) std::fprintf(stderr, "  screen:\n%s", lcd.c_str());
}

// CSAVE onto a blank tape, then CLOAD that recording into a fresh machine.
void test_csave_cload_round_trip() {
    const std::vector<uint8_t> image = readFile(kFixtureImg);
    PC1600Machine a;
    if (image.empty() || !bootWithCE1600P(a)) {
        std::fprintf(stderr, "SKIP test_csave_cload_round_trip: PC-1600 ROM images or fixture not found\n");
        return;
    }
    type(a, "NEW0");
    std::string error;
    CHECK(a.tapePlay(kFixtureWav, error));
    type(a, "CLOAD");
    a.tapeRecord("");
    type(a, "CSAVE \"ROUNDTRIP\"");
    CHECK(screen(a).find("ERROR") == std::string::npos);
    const std::vector<int16_t> recording = a.tapeDeck().recording();
    CHECK(recording.size() > 10 * TapeDeck::kRecordSampleRate);

    PC1600Machine b;
    CHECK(bootWithCE1600P(b));
    type(b, "NEW0");
    b.tapeDeck().loadForPlay(asPlayback(recording), TapeDeck::kRecordSampleRate);
    type(b, "CLOAD \"ROUNDTRIP\"");
    const std::string lcd = screen(b);
    CHECK(lcd.find("ROUNDTRIP") != std::string::npos);
    CHECK(lcd.find("ERROR") == std::string::npos);
    CHECK(programIs(b, image));

    // CLOAD? compares the tape with memory.
    b.tapeDeck().loadForPlay(asPlayback(recording), TapeDeck::kRecordSampleRate);
    type(b, "CLOAD? \"ROUNDTRIP\"");
    CHECK(screen(b).find("ERROR") == std::string::npos);

    // A tape with a dropout in its data doesn't load quietly. (Inverting
    // the signal wouldn't do: the ROM classifies either half-cycle the
    // same way.) The data block ends where the signal does; 0.2 s of
    // silence a second before that is well inside it.
    std::vector<int16_t> damaged = recording;
    size_t end = damaged.size();
    while (end > 0 && std::abs(damaged[end - 1]) < 1000) --end;
    const size_t rate = TapeDeck::kRecordSampleRate;
    CHECK(end > 2 * rate);
    for (size_t i = end - rate; i < end - rate + rate / 5; ++i) damaged[i] = 0;
    b.tapeDeck().loadForPlay(asPlayback(damaged), TapeDeck::kRecordSampleRate);
    type(b, "CLOAD? \"ROUNDTRIP\"");
    CHECK(screen(b).find("ERROR") != std::string::npos);
}

// The recording is a function of emulated time only: running the same
// CSAVE in small or large slices (as GUI pacing, turbo and the CLI do)
// writes the identical tape.
std::vector<int16_t> csaveInSlices(uint64_t slice) {
    PC1600Machine m;
    if (!bootWithCE1600P(m)) return {};
    std::string err;
    typeLine(m, "10 PRINT \"SPEED\"", true, &err);
    waitUntilBasicIdle(m, 10 * kSecond);
    m.tapeRecord("");
    typeLine(m, "CSAVE \"S\"", true, &err);
    for (uint64_t run = 0; run < 30 * kSecond; run += slice) m.runCycles(slice);
    return m.tapeDeck().recording();
}

void test_recording_independent_of_pacing() {
    const std::vector<int16_t> small = csaveInSlices(997);
    if (small.empty()) {
        std::fprintf(stderr, "SKIP test_recording_independent_of_pacing: PC-1600 ROM images not found\n");
        return;
    }
    const std::vector<int16_t> large = csaveInSlices(kSecond / 3);
    CHECK(small.size() > 5 * TapeDeck::kRecordSampleRate);
    const size_t n = std::min(small.size(), large.size());
    CHECK(std::equal(small.begin(), small.begin() + n, large.begin()));
}

} // namespace

int run_pc1600_tape_tests() {
    test_cload_bin2wav_tape();
    test_csave_cload_round_trip();
    test_recording_independent_of_pacing();
    std::printf("pc1600_tape_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
