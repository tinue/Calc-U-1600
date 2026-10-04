// Headless C++ tests for Core/Tape/TapeDeck.hpp (the WAV-backed cassette
// recorder) and Core/Audio/WavFile.hpp's reader. Synthetic only -- the
// machine-level CSAVE/CLOAD tests drive the real ROMs elsewhere.
//
// Build & run: see tools/run_tests.sh

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../Audio/WavFile.hpp"
#include "../Tape/TapeDeck.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

constexpr double kPi = 3.14159265358979323846;
constexpr double kCpuHz = 3580000.0; // PC-1600 T-states

std::string tempPath(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

void writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

// A RIFF/WAVE file with an arbitrary fmt chunk -- for the encodings
// writeWavMono16() doesn't produce.
std::vector<uint8_t> wavBytes(uint16_t format, uint16_t channels, uint32_t rate, uint16_t bits,
                              const std::vector<uint8_t>& data) {
    std::vector<uint8_t> b;
    auto put = [&](const char* s) { b.insert(b.end(), s, s + 4); };
    auto u16 = [&](uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); };
    auto u32 = [&](uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); };
    put("RIFF"); u32(uint32_t(4 + 8 + 16 + 8 + 8 + 2 + data.size()));
    put("WAVE");
    put("LIST"); u32(2); u16(0); // an unrelated chunk the reader must skip
    put("fmt "); u32(16);
    u16(format); u16(channels); u32(rate);
    u32(rate * channels * (bits / 8)); u16(uint16_t(channels * (bits / 8))); u16(bits);
    put("data"); u32(uint32_t(data.size()));
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

// Level changes of the deck's input line while playing, in CPU cycles.
std::vector<uint64_t> inputEdges(TapeDeck& deck, uint64_t totalCycles, uint64_t step = 20) {
    std::vector<uint64_t> edges;
    bool level = deck.inputLevel();
    for (uint64_t t = 0; t < totalCycles; t += step) {
        deck.advance(step);
        const bool now = deck.inputLevel();
        if (now != level) edges.push_back(t + step);
        level = now;
    }
    return edges;
}

// Samples of an FSK tone: `cycles` full periods at `hz`, sine or square.
void appendTone(std::vector<float>& out, int rate, double hz, int cycles, double amplitude, bool square,
                double offset = 0.0) {
    const int n = static_cast<int>(std::lround(cycles * rate / hz));
    for (int i = 0; i < n; ++i) {
        const double s = std::sin(2.0 * kPi * hz * i / rate);
        out.push_back(float(offset + amplitude * (square ? (s >= 0 ? 1.0 : -1.0) : s)));
    }
}

void testWavRoundTrip16() {
    const std::string path = tempPath("calcu_tape_rt16.wav");
    std::vector<int16_t> samples = {0, 1000, -1000, 32767, -32768, 12345};
    CHECK(writeWavMono16(path, samples, 44100));
    WavData wav;
    std::string error;
    CHECK(readWav(path, wav, error));
    CHECK(wav.sampleRate == 44100);
    CHECK(wav.samples.size() == samples.size());
    for (size_t i = 0; i < samples.size() && i < wav.samples.size(); ++i)
        CHECK(near(wav.samples[i], samples[i] / 32768.0, 1e-6));
    std::filesystem::remove(path);
}

void testWavEncodings() {
    const std::string path = tempPath("calcu_tape_enc.wav");
    WavData wav;
    std::string error;

    // 8-bit unsigned stereo: mixed down, 0x80 is silence.
    writeBytes(path, wavBytes(1, 2, 48000, 8, {0x80, 0x80, 0xFF, 0x01, 0xC0, 0xC0}));
    CHECK(readWav(path, wav, error));
    CHECK(wav.samples.size() == 3);
    if (wav.samples.size() == 3) {
        CHECK(near(wav.samples[0], 0.0, 1e-6));
        CHECK(near(wav.samples[1], 0.0, 1e-6)); // +127 and -127 cancel
        CHECK(near(wav.samples[2], 0.5, 1e-6));
    }

    // 24-bit mono, negative full scale.
    writeBytes(path, wavBytes(1, 1, 22050, 24, {0x00, 0x00, 0x80, 0x00, 0x00, 0x40}));
    CHECK(readWav(path, wav, error));
    CHECK(wav.sampleRate == 22050);
    if (wav.samples.size() == 2) {
        CHECK(near(wav.samples[0], -1.0, 1e-6));
        CHECK(near(wav.samples[1], 0.5, 1e-6));
    }

    // 32-bit float.
    const float f = -0.25f;
    std::vector<uint8_t> fdata(4);
    std::memcpy(fdata.data(), &f, 4);
    writeBytes(path, wavBytes(3, 1, 48000, 32, fdata));
    CHECK(readWav(path, wav, error));
    CHECK(wav.samples.size() == 1 && near(wav.samples[0], -0.25, 1e-6));

    // Unsupported: ADPCM.
    writeBytes(path, wavBytes(2, 1, 48000, 4, {0, 0}));
    CHECK(!readWav(path, wav, error));
    CHECK(!error.empty());

    // Not a WAV at all.
    writeBytes(path, {'h', 'e', 'l', 'l', 'o'});
    CHECK(!readWav(path, wav, error));
    CHECK(!readWav(tempPath("calcu_tape_missing.wav"), wav, error));
    std::filesystem::remove(path);
}

// A noisy, offset, 8-bit-like FSK burst plays back as clean edges at the
// tone's half periods; the silence after it doesn't toggle the line.
void testPlayFsk() {
    const int rate = 44100;
    std::vector<float> s;
    appendTone(s, rate, 1270.0, 40, 0.0, true);        // leading silence (amplitude 0)
    appendTone(s, rate, 2540.0, 200, 0.6, false, 0.1); // sine with DC offset
    appendTone(s, rate, 1270.0, 100, 0.6, true, -0.1); // square
    for (size_t i = 0; i < s.size(); ++i) s[i] += float(0.005 * std::sin(i * 1.7)); // hiss
    const size_t toneEnd = s.size();
    appendTone(s, rate, 1000.0, 200, 0.0, true); // trailing silence
    for (size_t i = toneEnd; i < s.size(); ++i) s[i] += float(0.005 * std::sin(i * 1.7));

    TapeDeck deck(kCpuHz);
    deck.loadForPlay(s, rate);
    CHECK(deck.mode() == TapeDeck::Mode::Play);
    deck.setMotor(true);
    const uint64_t total = static_cast<uint64_t>(double(s.size()) / rate * kCpuHz);
    const auto edges = inputEdges(deck, total);

    // Half periods, in seconds, measured from consecutive edges.
    int highTone = 0, lowTone = 0, other = 0;
    for (size_t i = 1; i < edges.size(); ++i) {
        const double half = (edges[i] - edges[i - 1]) / kCpuHz;
        if (near(half, 0.5 / 2540.0, 0.5 / 2540.0 * 0.2)) ++highTone;
        else if (near(half, 0.5 / 1270.0, 0.5 / 1270.0 * 0.2)) ++lowTone;
        else ++other;
    }
    CHECK(highTone >= 390);
    CHECK(lowTone >= 195);
    CHECK(other <= 3); // the transitions between tones and silence
    // Nothing toggles once the tone has stopped.
    const uint64_t toneEndCycles = static_cast<uint64_t>(double(toneEnd) / rate * kCpuHz);
    CHECK(!edges.empty() && edges.back() <= toneEndCycles + 2000);
    CHECK(deck.atEnd());
}

// Motor off: the tape doesn't move and nothing is recorded.
void testMotorGating() {
    TapeDeck deck(kCpuHz);
    std::vector<float> s;
    appendTone(s, 48000, 1200.0, 50, 0.8, true);
    deck.loadForPlay(s, 48000);
    deck.advance(1000000);
    CHECK(deck.positionSeconds() == 0.0);
    deck.setMotor(true);
    deck.advance(358000);
    CHECK(near(deck.positionSeconds(), 0.1, 1e-9));
    deck.setMotor(false);
    deck.advance(358000);
    CHECK(near(deck.positionSeconds(), 0.1, 1e-9));

    deck.armRecord("");
    CHECK(deck.positionSeconds() == 0.0);
    deck.advance(1000000);
    CHECK(deck.recording().empty());
}

// Toggle the output line at `hz` for `seconds`, in CPU cycles.
void driveTone(TapeDeck& deck, double hz, double seconds) {
    const double half = kCpuHz / hz / 2.0;
    const int halves = static_cast<int>(std::lround(seconds * hz * 2.0));
    double carry = 0.0;
    bool level = true;
    for (int i = 0; i < halves; ++i) {
        deck.setOutputLevel(level);
        carry += half;
        const uint64_t whole = static_cast<uint64_t>(carry);
        carry -= double(whole);
        deck.advance(whole);
        level = !level;
    }
}

int zeroCrossings(const std::vector<int16_t>& s, size_t from, size_t to) {
    int n = 0;
    for (size_t i = from + 1; i < to && i < s.size(); ++i)
        if ((s[i - 1] < 0) != (s[i] < 0)) ++n;
    return n;
}

// Recording: 48 kHz PCM of the line at the right pitch, only while the
// motor runs; eject() writes a WAV that reads back the same.
void testRecord() {
    const std::string path = tempPath("calcu_tape_rec.wav");
    TapeDeck deck(kCpuHz);
    deck.armRecord(path);
    deck.setMotor(true);
    driveTone(deck, 3000.0, 0.5);
    deck.setMotor(false);
    driveTone(deck, 1200.0, 0.5); // motor off: not on the tape
    deck.setMotor(true);
    driveTone(deck, 1200.0, 0.5);
    const auto& rec = deck.recording();
    CHECK(near(double(rec.size()), 48000.0, 2.0));
    // ~3000 Hz in the first half second, ~1200 Hz in the second.
    CHECK(near(zeroCrossings(rec, 1000, 24000), 2 * 3000 * 23000 / 48000.0, 10));
    CHECK(near(zeroCrossings(rec, 25000, 48000), 2 * 1200 * 23000 / 48000.0, 10));
    int16_t peak = 0;
    for (int16_t v : rec) peak = std::max<int16_t>(peak, static_cast<int16_t>(std::abs(v)));
    CHECK(peak > 16000 && peak < 32767);

    CHECK(deck.eject());
    CHECK(deck.mode() == TapeDeck::Mode::Empty);
    WavData wav;
    std::string error;
    CHECK(readWav(path, wav, error));
    CHECK(wav.sampleRate == TapeDeck::kRecordSampleRate);
    CHECK(wav.samples.size() == deck.recording().size());
    std::filesystem::remove(path);

    // A recording that can't be written reports it.
    TapeDeck bad(kCpuHz);
    bad.armRecord("/nonexistent-dir/x.wav");
    std::string why;
    CHECK(!bad.eject(&why));
    CHECK(!why.empty());
}

// What one deck records, another plays back with the same edge timing.
void testRecordThenPlay() {
    TapeDeck rec(kCpuHz);
    rec.armRecord("");
    rec.setMotor(true);
    driveTone(rec, 1200.0, 0.2);
    driveTone(rec, 3000.0, 0.2);

    std::vector<float> samples;
    for (int16_t v : rec.recording()) samples.push_back(v / 32768.0f);
    TapeDeck play(kCpuHz);
    play.loadForPlay(samples, TapeDeck::kRecordSampleRate);
    play.setMotor(true);
    const auto edges = inputEdges(play, static_cast<uint64_t>(0.4 * kCpuHz));
    int slow = 0, fast = 0;
    for (size_t i = 1; i < edges.size(); ++i) {
        const double half = (edges[i] - edges[i - 1]) / kCpuHz;
        // One 48 kHz sample is ~75 T-states of jitter on each edge.
        if (near(half, 0.5 / 1200.0, 2.0 / 48000.0)) ++slow;
        else if (near(half, 0.5 / 3000.0, 2.0 / 48000.0)) ++fast;
    }
    CHECK(slow >= 470);
    CHECK(fast >= 1190);
}

} // namespace

int run_tape_deck_tests() {
    testWavRoundTrip16();
    testWavEncodings();
    testPlayFsk();
    testMotorGating();
    testRecord();
    testRecordThenPlay();
    std::printf("tape_deck_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
