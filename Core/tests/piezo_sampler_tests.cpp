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

#include <algorithm>
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
#include "../PC1600/PC1600Memory.hpp"
#include "TestRoms.hpp"

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
    double periods = 0.0;      // full square-wave cycles seen
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
    size_t crossings = 0, firstCrossing = 0, lastCrossing = 0;
    for (size_t i = first; i <= last; ++i) {
        bool crossed = false;
        if (sign > 0 && pcm[i] < -kAudible / 2) { sign = -1; crossed = true; }
        else if (sign < 0 && pcm[i] > kAudible / 2) { sign = 1; crossed = true; }
        if (crossed) {
            if (crossings++ == 0) firstCrossing = i;
            lastCrossing = i;
        }
    }
    // Pitch from the span between the first and last crossing, so the DC
    // blocker's decay tail after the last edge doesn't dilute it.
    t.periods = crossings / 2.0;
    if (crossings > 2)
        t.hz = ((crossings - 1) / 2.0) / (static_cast<double>(lastCrossing - firstCrossing) / kRate);
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

// DFT magnitude of `pcm` at `hz` (single bin, Hann window).
double toneLevel(const std::vector<int16_t>& pcm, double hz) {
    double re = 0.0, im = 0.0;
    const size_t n = pcm.size();
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / (n - 1));
        const double ph = 2.0 * 3.14159265358979 * hz * i / kRate;
        re += w * pcm[i] * std::cos(ph);
        im += w * pcm[i] * std::sin(ph);
    }
    return std::sqrt(re * re + im * im);
}

// The PC-1600 transducer model reproduces the real buzzer's shape: a
// 287 Hz square (BEEP A=200) comes out with its fundamental far below the
// 2 kHz 7th harmonic (real unit: ~35 dB), while the raw line keeps the
// square wave's own 1/7 (-17 dB) ratio the other way round.
void test_pc1600_transducer_shape() {
    const double cpuHz = 3580000.0;
    const uint32_t halfPeriod = 6221; // 60*200+441 T per period, /2
    PiezoSampler raw(cpuHz), piezo(cpuHz, PiezoSampler::Transducer::PC1600);
    bool level = false;
    for (int i = 0; i < 2 * 287; ++i) { // ~1 s
        level = !level;
        raw.setLevel(level);
        piezo.setLevel(level);
        raw.advance(halfPeriod);
        piezo.advance(halfPeriod);
    }
    std::vector<int16_t> r = drainAll(raw), p = drainAll(piezo);
    r.erase(r.begin(), r.begin() + kRate / 10); // skip the filters' settling
    p.erase(p.begin(), p.begin() + kRate / 10);
    const double f0 = cpuHz / (2.0 * halfPeriod);
    const double rawDb = 20.0 * std::log10(toneLevel(r, f0) / toneLevel(r, 7 * f0));
    const double piezoDb = 20.0 * std::log10(toneLevel(p, f0) / toneLevel(p, 7 * f0));
    std::printf("  PC-1600 transducer, fundamental vs 7th harmonic: raw %+.1f dB, piezo %+.1f dB\n",
                rawDb, piezoDb);
    CHECK(rawDb > 15.0);
    CHECK(piezoDb < -25.0);
    // No clipping even at the resonance (BEEP A=20, ~2.2 kHz).
    PiezoSampler loud(cpuHz, PiezoSampler::Transducer::PC1600);
    for (int i = 0; i < 2 * 2182; ++i) { level = !level; loud.setLevel(level); loud.advance(820); }
    int peak = 0;
    for (int16_t v : drainAll(loud)) peak = std::max(peak, std::abs(static_cast<int>(v)));
    CHECK(peak < 32000);
}

// The PC-1500 transducer model: a 551 Hz square (BEEP A=100) is heard
// through its 7th harmonic at 3.9 kHz, which the real unit's sweep puts
// ~26 dB above the fundamental; the raw line has the fundamental 17 dB up.
void test_pc1500_transducer_shape() {
    const double cpuHz = 1300000.0;
    const uint32_t halfPeriod = 1181; // (161 + 22*100) cycles per period, /2
    PiezoSampler raw(cpuHz), piezo(cpuHz, PiezoSampler::Transducer::PC1500);
    bool level = false;
    for (int i = 0; i < 2 * 550; ++i) { // ~1 s
        level = !level;
        raw.setLevel(level);
        piezo.setLevel(level);
        raw.advance(halfPeriod);
        piezo.advance(halfPeriod);
    }
    std::vector<int16_t> r = drainAll(raw), p = drainAll(piezo);
    r.erase(r.begin(), r.begin() + kRate / 10); // skip the filters' settling
    p.erase(p.begin(), p.begin() + kRate / 10);
    const double f0 = cpuHz / (2.0 * halfPeriod);
    const double rawDb = 20.0 * std::log10(toneLevel(r, f0) / toneLevel(r, 7 * f0));
    const double piezoDb = 20.0 * std::log10(toneLevel(p, f0) / toneLevel(p, 7 * f0));
    std::printf("  PC-1500 transducer, fundamental vs 7th harmonic: raw %+.1f dB, piezo %+.1f dB\n",
                rawDb, piezoDb);
    CHECK(rawDb > 15.0);
    CHECK(piezoDb < -20.0);
    // No clipping even on the 6 kHz resonance (BEEP A=2, 6.34 kHz).
    PiezoSampler loud(cpuHz, PiezoSampler::Transducer::PC1500);
    for (int i = 0; i < 2 * 6341; ++i) { level = !level; loud.setLevel(level); loud.advance(102); }
    int peak = 0;
    for (int16_t v : drainAll(loud)) peak = std::max(peak, std::abs(static_cast<int>(v)));
    CHECK(peak < 32000);
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
    // Measure the ROM's drive signal, not the buzzer's acoustic response.
    m.memory().piezo().setTransducer(PiezoSampler::Transducer::None);
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
    // Measure the ROM's drive signal, not the buzzer's acoustic response.
    m.memory().piezo().setTransducer(PiezoSampler::Transducer::None);
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
    CHECK(near(p.periods, 500.0, 0.01));
    // Pitch vs the ROM loop's cycle count with the SC-7852's M1 wait:
    // 60*A + 441 T-states per period (1260 Hz for A = 40), which a real
    // unit matches to 0.2% (SC7852.hpp). The TRM 3.10 formula
    // 1 300 000 / (166 + 22*A) is Sharp's rounded version of it.
    CHECK(near(p.hz, 3580000.0 / (60 * 40 + 441), 0.01));
    CHECK(near(p.hz, 1300000.0 / (166 + 22 * 40), 0.03));

    // Hardware reference (2026-09-23): BEEP 1,200,1000 on a real PC-1600
    // measured 287.13 Hz.
    ToneStats hw = analyse(runAndCapture(m, "BEEP 1,200,300"));
    std::printf("  PC-1600 BEEP 1,200,300: %.1f ms at %.1f Hz\n",
                1000.0 * hw.audibleSamples / kRate, hw.hz);
    CHECK(near(hw.hz, 287.13, 0.01));

    runAndCapture(m, "BEEP OFF");
    CHECK(analyse(runAndCapture(m, "BEEP 1")).audibleSamples == 0);
    runAndCapture(m, "BEEP ON");
    CHECK(analyse(runAndCapture(m, "BEEP 1")).audibleSamples > 0);
}

// BEEP n,A,d repeats are paced by the ROM counting 64 Hz PB5 rising edges
// (P1-B3 5F12): period = (PB5 ticks the tone spans + 5) / 64 s. A real unit
// plays BEEP 20,200,20 at a steady 156.25 ms (10 ticks). Two things used to
// break that: nominal timing (tone too short -> 9 ticks) and the slow
// PB5-synced sub-CPU path in the 0.5 s ISR, which swallowed edges (+1 tick
// about every other beep). The boot's IOCS 25H probe must leave F0B8H
// bit 0 set (fast path).
void test_pc1600_beep_repeat_spacing() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_pc1600_beep_repeat_spacing: PC-1600 ROM images not found\n");
        return;
    }
    CHECK((m.memory().read(0xF0B8) & 0x01) != 0);
    m.memory().piezo().setTransducer(PiezoSampler::Transducer::None);

    const std::vector<int16_t> pcm = runAndCapture(m, "BEEP 12,200,20");
    std::vector<size_t> starts;
    size_t quiet = kRate; // samples since the last audible one
    for (size_t i = 0; i < pcm.size(); ++i) {
        if (std::abs(pcm[i]) >= kAudible) {
            if (quiet >= static_cast<size_t>(kRate / 250)) starts.push_back(i); // >= 4 ms of silence
            quiet = 0;
        } else {
            ++quiet;
        }
    }
    CHECK(starts.size() == 12);
    // The first tone starts whenever the command gets there, not on a
    // PB5 edge, so its gap is partial. Every one after that is exact.
    for (size_t i = 2; i < starts.size(); ++i) {
        const double ms = 1000.0 * static_cast<double>(starts[i] - starts[i - 1]) / kRate;
        if (!near(ms, 156.25, 0.005)) std::printf("  BEEP 12,200,20 period %zu: %.2f ms\n", i, ms);
        CHECK(near(ms, 156.25, 0.005));
    }
}

// Port 17H, the LH5810-style F register: F6 = 1 puts the modulation clock
// FX on SDO, which drives the buzzer alongside OPC. dampflok.bas whistles
// with F = 41H (FX = phi/128), measured at 2539 Hz on a real unit, so
// phi = 1.3 MHz / 4.
void test_pc1600_f_register_modulator() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_pc1600_f_register_modulator: PC-1600 ROM images not found\n");
        return;
    }
    m.memory().piezo().setTransducer(PiezoSampler::Transducer::None);
    ToneStats on = analyse(runAndCapture(m, "OUT 23,65"));
    std::printf("  PC-1600 OUT 23,65: %.1f ms at %.1f Hz\n", 1000.0 * on.audibleSamples / kRate, on.hz);
    CHECK(near(on.hz, 1300000.0 / 512, 0.003));
    CHECK(on.audibleSamples > static_cast<size_t>(kRate * 2)); // keeps sounding
    // Modulation off: silent again (bar the DC step's brief decay).
    CHECK(analyse(runAndCapture(m, "OUT 23,0")).audibleSamples < static_cast<size_t>(kRate / 20));
    // FX = phi/512 (F0-2 = 011): 635 Hz.
    CHECK(near(analyse(runAndCapture(m, "OUT 23,67")).hz, 1300000.0 / 2048, 0.003));
    runAndCapture(m, "OUT 23,0");
    // BEEP OFF gates it like the BEEP tone.
    runAndCapture(m, "BEEP OFF");
    CHECK(analyse(runAndCapture(m, "OUT 23,65")).audibleSamples < static_cast<size_t>(kRate / 20));
    runAndCapture(m, "OUT 23,0");
    runAndCapture(m, "BEEP ON");
}

} // namespace

int run_piezo_sampler_tests() {
    test_square_wave_pitch();
    test_constant_level_decays_to_silence();
    test_mid_sample_edge_is_averaged();
    test_overflow_keeps_newest();
    test_pc1600_transducer_shape();
    test_pc1500_transducer_shape();
    test_pc1500_beep();
    test_pc1500_settle_waits_for_beep();
    test_pc1600_beep();
    test_pc1600_beep_repeat_spacing();
    test_pc1600_f_register_modulator();

    std::printf("piezo_sampler_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
