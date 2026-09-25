#include "PiezoSampler.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace {
// DC-blocker corner. Well under the buzzer's lowest audible tones, high
// enough that a parked line settles within a fraction of a second.
constexpr double kHighPassHz = 20.0;
// A full-swing square wave comes out of the high-pass at about +-0.5;
// scaled to ~+-0.3 of full scale so the (deliberately raw) square wave
// isn't painfully loud.
constexpr double kGain = 0.6;
constexpr double kPi = 3.14159265358979323846;

// ── PC-1600 buzzer acoustic response ─────────────────────────────────────
//
// Fitted (least squares, 5 dB rms) to the harmonic levels of real-unit
// microphone recordings of BEEP 1,200,1000 / BEEP 5,200,100 (287 Hz) and
// BEEP 5,50,100 (1038 Hz), each harmonic corrected for the square wave's
// own 1/k fall-off (2026-09-23). Relative to the 2-3 kHz peak the real
// buzzer sits at -52 dB at 287 Hz, -21 dB at 861 Hz, -7 dB at 1.4 kHz,
// then falls to -10..-25 dB over 5-7 kHz. Three RBJ biquads reproduce
// that shape:
//   - a resonant high-pass at 2.1 kHz (Q 3.1): the disc's main resonance
//     plus the steep fall below it,
//   - a second-order high-pass at 520 Hz (Q 0.65),
//   - an overdamped low-pass at 7.3 kHz (Q 0.3): the gentle top roll-off.
// Known gap: the one 1038 Hz fundamental measured ~12 dB above this fit.
// A pitch sweep recorded on the real unit would pin down 0.8-1.4 kHz.
enum class Stage { HighPass, LowPass, Peak };
struct BiquadSpec {
    Stage kind;
    double hz;
    double q;
    double gainDb = 0.0; // Peak only
};
constexpr BiquadSpec kPC1600Stages[] = {
    {Stage::HighPass, 2115.0, 3.07},
    {Stage::HighPass, 521.0, 0.654},
    {Stage::LowPass, 7295.0, 0.30},
};
// Output gain after the cascade. The 2.1 kHz resonance makes a tone near
// it (BEEP A=20) ~15 dB louder than a low one, as on the real unit. 0.5
// keeps that one at about -2 dBFS; a typical A=100-255 tone then sits
// around -20 dBFS rms.
constexpr double kPC1600Gain = 0.5;

// ── PC-1500 buzzer acoustic response ─────────────────────────────────────
//
// Fitted (least squares, 3 dB rms over 0.5-15 kHz) to a real-unit
// recording of a BEEP sweep: tones A = 0..255 step 3, 250 ms each, every
// odd harmonic up to 15 kHz corrected for the square wave's 1/k fall-off
// -- ~2000 points, binned to 1/6 octave (2026-09-23). Relative to the
// 6 kHz peak the real buzzer sits at -50 dB at 530 Hz, -36 dB at 1.1 kHz,
// -12 dB at 2.7 kHz, has a narrow resonance at 3.9 kHz (-3 dB) and a
// broader one at 6.1 kHz, then a -12..-18 dB plateau up to 15 kHz.
// Below ~500 Hz the recording hits its noise floor (< -55 dB). So a low
// BEEP is heard almost entirely through its harmonics: the 7th for A=100,
// the 17th for A=255. Three RBJ biquads:
//   - a resonant high-pass at 3.26 kHz (Q 1.9): the steep ~12 dB/oct
//     rise below the resonances,
//   - a peak at 3.94 kHz (Q 8, +8.6 dB),
//   - a peak at 6.17 kHz (Q 3, +15.2 dB).
constexpr BiquadSpec kPC1500Stages[] = {
    {Stage::HighPass, 3264.0, 1.91},
    {Stage::Peak, 3937.0, 8.0, 8.56},
    {Stage::Peak, 6168.0, 3.0, 15.24},
};
// Puts the loudest tone (A=2, 6.3 kHz, on the main resonance) at about
// -2 dBFS; BEEP's default A=8 sits near -7 dBFS rms and a typical
// A=100-255 tone around -20 dBFS rms, as with the PC-1600.
constexpr double kPC1500Gain = 0.18;
} // namespace

PiezoSampler::PiezoSampler(double cpuHz, Transducer transducer, int sampleRate)
    : m_cyclesPerSample(cpuHz / sampleRate),
      m_sampleRate(sampleRate),
      m_hpCoeff(1.0 - 2.0 * kPi * kHighPassHz / sampleRate),
      m_ring(static_cast<size_t>(sampleRate)) {
    setTransducer(transducer);
}

void PiezoSampler::setTransducer(Transducer transducer) {
    m_stageCount = 0;
    m_gain = kGain;
    const BiquadSpec* specs = nullptr;
    int count = 0;
    double gain = kGain;
    switch (transducer) {
        case Transducer::None: return;
        case Transducer::PC1500:
            specs = kPC1500Stages;
            count = static_cast<int>(std::size(kPC1500Stages));
            gain = kPC1500Gain;
            break;
        case Transducer::PC1600:
            specs = kPC1600Stages;
            count = static_cast<int>(std::size(kPC1600Stages));
            gain = kPC1600Gain;
            break;
    }
    for (int i = 0; i < count; ++i) {
        const BiquadSpec& spec = specs[i];
        // RBJ Audio EQ Cookbook high-/low-pass and peaking EQ, normalized by a0.
        const double w = 2.0 * kPi * spec.hz / m_sampleRate;
        const double alpha = std::sin(w) / (2.0 * spec.q);
        const double c = std::cos(w);
        Biquad& bq = m_stages[m_stageCount++];
        bq = Biquad{};
        double b0, b1, b2, a0, a1, a2;
        if (spec.kind == Stage::Peak) {
            const double amp = std::pow(10.0, spec.gainDb / 40.0);
            b0 = 1.0 + alpha * amp;
            b1 = -2.0 * c;
            b2 = 1.0 - alpha * amp;
            a0 = 1.0 + alpha / amp;
            a1 = -2.0 * c;
            a2 = 1.0 - alpha / amp;
        } else {
            const bool highPass = spec.kind == Stage::HighPass;
            const double edge = highPass ? (1.0 + c) / 2.0 : (1.0 - c) / 2.0;
            b0 = edge;
            b1 = highPass ? -2.0 * edge : 2.0 * edge;
            b2 = edge;
            a0 = 1.0 + alpha;
            a1 = -2.0 * c;
            a2 = 1.0 - alpha;
        }
        bq.b0 = b0 / a0;
        bq.b1 = b1 / a0;
        bq.b2 = b2 / a0;
        bq.a1 = a1 / a0;
        bq.a2 = a2 / a0;
    }
    m_gain = gain;
}

void PiezoSampler::emitSample() {
    const double x = m_area / m_cyclesPerSample; // 0..1 duty over the interval
    m_phase = 0.0;
    m_area = 0.0;
    double y = x - m_hpPrevIn + m_hpCoeff * m_hpPrevOut;
    m_hpPrevIn = x;
    m_hpPrevOut = y;
    for (int i = 0; i < m_stageCount; ++i) y = m_stages[i].process(y);

    const double scaled = std::clamp(y * m_gain, -1.0, 1.0) * 32767.0;
    const int16_t s = static_cast<int16_t>(std::lround(scaled));
    const size_t cap = m_ring.size();
    if (m_count == cap) {           // full: drop the oldest
        m_head = (m_head + 1) % cap;
        --m_count;
    }
    m_ring[(m_head + m_count) % cap] = s;
    ++m_count;
}

size_t PiezoSampler::drain(int16_t* out, size_t max) {
    const size_t n = std::min(max, m_count);
    const size_t cap = m_ring.size();
    for (size_t i = 0; i < n; ++i) out[i] = m_ring[(m_head + i) % cap];
    m_head = (m_head + n) % cap;
    m_count -= n;
    return n;
}
