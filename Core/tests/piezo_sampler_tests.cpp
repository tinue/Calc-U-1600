// Headless C++ tests for Core/Audio/PiezoSampler.cpp and for the buzzer
// drive lines feeding it: PC-1500 LH5811 OPC bit 6 (PC6) and PC-1600
// SC-7852 OPC port 18H (bit 7 wave, bit 6 gate).
//
// The synthetic tests need no ROM. The machine tests type a real BEEP at
// the ROM's BASIC prompt and analyse the PCM that falls out of the ROM's
// own bit-banging loop -- they SKIP (not fail) when the ROM images are
// absent from their repo-root paths.
//
// Build & run: see tools/run_tests.sh

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../Audio/PiezoSampler.hpp"
#include "../PC1500/PC1500BasicTyper.hpp"
#include "../PC1500/PC1500Machine.hpp"
#include "../PC1600/PC1600BasicTyper.hpp"
#include "../PC1600/PC1600Machine.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

constexpr int kRate = PiezoSampler::kDefaultSampleRate;
constexpr int16_t kAudible = 2000; // well above the DC blocker's decay tail

std::vector<int16_t> drainAll(PiezoSampler& s) {
    std::vector<int16_t> out(s.available());
    out.resize(s.drain(out.data(), out.size()));
    return out;
}

// What a chunk of buzzer audio sounds like: how long it is audible and at
// what pitch. The pitch comes from counting zero crossings (with
// hysteresis) between the first and last audible samples, so it measures
// the square wave's fundamental without depending on its amplitude.
struct ToneStats {
    size_t audibleSamples = 0; // first..last audible sample, inclusive
    double hz = 0.0;
};

ToneStats analyse(const std::vector<int16_t>& pcm) {
    ToneStats t;
    size_t first = pcm.size(), last = 0;
    for (size_t i = 0; i < pcm.size(); ++i) {
        if (std::abs(pcm[i]) >= kAudible) {
            if (first == pcm.size()) first = i;
            last = i;
        }
    }
    if (first == pcm.size()) return t;
    t.audibleSamples = last - first + 1;
    int sign = pcm[first] >= 0 ? 1 : -1;
    size_t crossings = 0;
    for (size_t i = first; i <= last; ++i) {
        if (sign > 0 && pcm[i] < -kAudible / 2) { sign = -1; ++crossings; }
        else if (sign < 0 && pcm[i] > kAudible / 2) { sign = 1; ++crossings; }
    }
    t.hz = (crossings / 2.0) / (static_cast<double>(t.audibleSamples) / kRate);
    return t;
}

bool near(double a, double b, double tolerance) {
    return std::fabs(a - b) <= tolerance * b;
}

// ── Synthetic (no ROM) ──────────────────────────────────────────────────

// A square wave of known period, in CPU cycles, comes out at that pitch.
void test_square_wave_pitch() {
    const double cpuHz = 1300000.0;
    PiezoSampler s(cpuHz);
    const uint32_t halfPeriod = 650; // 1000 Hz
    bool level = false;
    for (int i = 0; i < 2000; ++i) { // 1 s
        level = !level;
        s.setLevel(level);
        s.advance(halfPeriod);
    }
    ToneStats t = analyse(drainAll(s));
    CHECK(near(t.hz, 1000.0, 0.01));
    CHECK(near(static_cast<double>(t.audibleSamples), kRate, 0.01));
}

// A line parked high (the ROM's idle level) decays to silence: the DC
// blocker stands in for the piezo's AC coupling.
void test_constant_level_decays_to_silence() {
    PiezoSampler s(3580000.0);
    s.setLevel(true);
    s.advance(3580000); // 1 s
    std::vector<int16_t> pcm = drainAll(s);
    CHECK(pcm.size() + 1 >= static_cast<size_t>(kRate)); // fractional-accumulator rounding
    CHECK(!pcm.empty() && pcm.front() > kAudible);   // the step itself
    CHECK(!pcm.empty() && std::abs(pcm.back()) < 50); // ...gone within 1 s
}

// An edge halfway through a sample interval yields the proportional
// average, not a hard 0 or 1.
void test_mid_sample_edge_is_averaged() {
    const double cpuHz = 48000.0 * 100; // exactly 100 cycles per sample
    PiezoSampler s(cpuHz);
    s.setLevel(false);
    s.advance(100);
    s.setLevel(true);
    s.advance(50);
    s.setLevel(false);
    s.advance(50);
    std::vector<int16_t> pcm = drainAll(s);
    CHECK(pcm.size() == 2);
    // Full-scale step through the DC blocker is 0.6 * 32767; half of that.
    CHECK(pcm.size() == 2 && std::abs(pcm[1] - 9830) < 50);
}

// The ring keeps only the newest ~1 s when nobody drains it.
void test_overflow_keeps_newest() {
    PiezoSampler s(48000.0); // 1 cycle per sample
    s.advance(kRate + 500);
    CHECK(s.available() == static_cast<size_t>(kRate));
    s.discard();
    CHECK(s.available() == 0);
}

// ── ROM-gated: PC-1500 ──────────────────────────────────────────────────

bool bootPC1500(PC1500Machine& m) {
    if (!m.loadROMFile("roms/PC-1500_A04.ROM")) return false;
    m.reset();
    m.runCycles(static_cast<uint64_t>(1300000.0 * 2));
    waitIdle(m, static_cast<uint64_t>(1300000.0 * 5));
    // A cold boot stops at "NEW0? :CHECK"; CL answers it, as
    // PC1500PresetLoader does. A second tap covers one missed while the
    // memory check is still finishing, and is harmless at the prompt.
    for (int i = 0; i < 2; ++i) {
        tapKey(m, "cl");
        m.runCycles(static_cast<uint64_t>(1300000.0 / 2));
    }
    return true;
}

// Types `line` and returns the audio the ROM produced while executing it.
std::vector<int16_t> runAndCapture(PC1500Machine& m, const std::string& line) {
    // Enter is tapped here rather than by typeLine(): typeLine() settles
    // for longer than the sampler's ~1 s ring after Enter, which would drop
    // a short BEEP before it could be drained.
    std::string err;
    std::vector<int16_t> pcm;
    if (!typeLine(m, line, /*pressEnter=*/false, &err)) return pcm;
    m.discardAudio();
    tapKey(m, "enter");
    std::vector<int16_t> chunk(kRate);
    for (int i = 0; i < 30; ++i) { // 3 s in 100 ms steps, draining as we go
        m.runCycles(130000);
        chunk.resize(m.drainAudio(chunk.data(), kRate));
        pcm.insert(pcm.end(), chunk.begin(), chunk.end());
        chunk.resize(kRate);
    }
    return pcm;
}

void test_pc1500_beep() {
    PC1500Machine m;
    if (!bootPC1500(m)) {
        std::fprintf(stderr, "SKIP test_pc1500_beep: PC-1500 ROM image not found\n");
        return;
    }
    ToneStats t = analyse(runAndCapture(m, "BEEP 1"));
    std::printf("  PC-1500 BEEP 1: %.1f ms at %.1f Hz\n",
                1000.0 * t.audibleSamples / kRate, t.hz);
    CHECK(t.audibleSamples > static_cast<size_t>(kRate / 50)); // > 20 ms
    CHECK(t.audibleSamples < static_cast<size_t>(kRate));      // < 1 s
    CHECK(t.hz > 1000.0 && t.hz < 8000.0);

    // BEEP OFF: the ROM stops driving PC6, so nothing is audible.
    runAndCapture(m, "BEEP OFF");
    ToneStats off = analyse(runAndCapture(m, "BEEP 1"));
    CHECK(off.audibleSamples == 0);
    runAndCapture(m, "BEEP ON");
    CHECK(analyse(runAndCapture(m, "BEEP 1")).audibleSamples > 0);
}

// typeLine()'s post-Enter settle must not mistake the BEEP tone loop (a
// small PC window high in ROM, like the idle prompt) for the prompt --
// otherwise the next typed line lands mid-beep and is mangled.
void test_pc1500_settle_waits_for_beep() {
    PC1500Machine m;
    if (!bootPC1500(m)) {
        std::fprintf(stderr, "SKIP test_pc1500_settle_waits_for_beep: PC-1500 ROM image not found\n");
        return;
    }
    std::string err;
    CHECK(typeLine(m, "BEEP 2,100,300", /*pressEnter=*/true, &err));
    const uint16_t pc = m.debugPC();
    CHECK(pc >= 0xE200 && pc <= 0xE4FF); // back at the prompt loop, not E6xx
    const uint64_t edges = m.buzzerEdgeCount();
    m.runCycles(130000); // 0.1 s more: the beep is really over
    CHECK(m.buzzerEdgeCount() == edges);
}

// ── ROM-gated: PC-1600 ──────────────────────────────────────────────────

bool readRomFile(const char* path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    *out = std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return !out->empty();
}

bool bootPC1600(PC1600Machine& m) {
    std::vector<uint8_t> i0, ii0, iii3, r3b, iv6, r1500;
    if (!readRomFile("roms/PC1600-P0-B0.bin", &i0) ||
        !readRomFile("roms/PC1600-P1-B0.bin", &ii0) ||
        !readRomFile("roms/PC1600-P1-B3.bin", &iii3) ||
        !readRomFile("roms/PC1600-P1-B3B.bin", &r3b) ||
        !readRomFile("roms/PC1600-P2-B6.bin", &iv6) ||
        !readRomFile("roms/PC1600-LH5803-C000-FFFF.bin", &r1500)) {
        return false;
    }
    if (!(m.loadBank0(i0.data(), i0.size(), ii0.data(), ii0.size()) &&
          m.loadBank3Rom(iii3.data(), iii3.size()) &&
          m.loadBank3bRom(r3b.data(), r3b.size()) &&
          m.loadBank6Rom(iv6.data(), iv6.size()) &&
          m.loadLH5803Rom(r1500.data(), r1500.size()))) {
        return false;
    }
    m.allReset();
    m.runCycles(static_cast<uint64_t>(PC1600Machine::kTStateHz) * 2);
    waitIdle(m, static_cast<uint64_t>(PC1600Machine::kTStateHz) * 5);
    return true;
}

std::vector<int16_t> runAndCapture(PC1600Machine& m, const std::string& line) {
    // Enter is tapped here rather than by typeLine(): typeLine() settles
    // for longer than the sampler's ~1 s ring after Enter, which would drop
    // a short BEEP before it could be drained.
    std::string err;
    std::vector<int16_t> pcm;
    if (!typeLine(m, line, /*pressEnter=*/false, &err)) return pcm;
    m.discardAudio();
    tapKey(m, "enter");
    std::vector<int16_t> chunk(kRate);
    for (int i = 0; i < 30; ++i) { // 3 s in 100 ms steps
        m.runCycles(PC1600Machine::kTStateHz / 10);
        chunk.resize(m.drainAudio(chunk.data(), kRate));
        pcm.insert(pcm.end(), chunk.begin(), chunk.end());
        chunk.resize(kRate);
    }
    return pcm;
}

void test_pc1600_beep() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_pc1600_beep: PC-1600 ROM images not found\n");
        return;
    }
    ToneStats t = analyse(runAndCapture(m, "BEEP 1"));
    std::printf("  PC-1600 BEEP 1: %.1f ms at %.1f Hz\n",
                1000.0 * t.audibleSamples / kRate, t.hz);
    CHECK(t.audibleSamples > static_cast<size_t>(kRate / 50));
    CHECK(t.audibleSamples < static_cast<size_t>(kRate));
    CHECK(t.hz > 1000.0 && t.hz < 8000.0);

    // Explicit pitch/duration: TRM 3.10 gives the BOUT tone as
    // 1 300 000 / (166 + 22*A) Hz and its length as BC / frequency.
    ToneStats p = analyse(runAndCapture(m, "BEEP 1,40,500"));
    std::printf("  PC-1600 BEEP 1,40,500: %.1f ms at %.1f Hz\n",
                1000.0 * p.audibleSamples / kRate, p.hz);
    // Exactly BC = 500 periods, independent of CPU timing.
    CHECK(near(p.hz * p.audibleSamples / kRate, 500.0, 0.02));
    // Pitch vs the TRM formula (1243 Hz for A = 40). The loop measures
    // ~12% high at the SC-7852's nominal Zilog timing: the formula matches
    // one wait state per M1 fetch within ~1%, which the BASIC-loop
    // hardware benchmark in PC-1600-CPU-SC7852-Z80.md 2.3 argues against.
    // Kept loose until that's settled on hardware.
    CHECK(near(p.hz, 1300000.0 / (166 + 22 * 40), 0.15));

    runAndCapture(m, "BEEP OFF");
    CHECK(analyse(runAndCapture(m, "BEEP 1")).audibleSamples == 0);
    runAndCapture(m, "BEEP ON");
    CHECK(analyse(runAndCapture(m, "BEEP 1")).audibleSamples > 0);
}

} // namespace

int run_piezo_sampler_tests() {
    test_square_wave_pitch();
    test_constant_level_decays_to_silence();
    test_mid_sample_edge_is_averaged();
    test_overflow_keeps_newest();
    test_pc1500_beep();
    test_pc1500_settle_waits_for_beep();
    test_pc1600_beep();

    std::printf("piezo_sampler_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
