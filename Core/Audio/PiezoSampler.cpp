#include "PiezoSampler.hpp"

#include <algorithm>
#include <cmath>

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
struct BiquadSpec {
    bool highPass;
    double hz;
    double q;
};
constexpr BiquadSpec kPC1600Stages[] = {
    {true, 2115.0, 3.07},
    {true, 521.0, 0.654},
    {false, 7295.0, 0.30},
};
// Output gain after the cascade. The 2.1 kHz resonance makes a tone near
// it (BEEP A=20) ~15 dB louder than a low one, as on the real unit. 0.5
// keeps that one at about -2 dBFS; a typical A=100-255 tone then sits
// around -20 dBFS rms.
constexpr double kPC1600Gain = 0.5;
} // namespace

PiezoSampler::PiezoSampler(double cpuHz, Transducer transducer, int sampleRate)
    : m_cyclesPerSample(cpuHz / sampleRate),
      m_sampleRate(sampleRate),
      m_hpCoeff(1.0 - 2.0 * kPi * kHighPassHz / sampleRate),
      m_ring(static_cast<size_t>(sampleRate)) {
    setTransducer(transducer);
}

void PiezoSampler::setTransducer(Transducer transducer) {
    m_transducer = transducer;
    m_stageCount = 0;
    m_gain = kGain;
    if (transducer != Transducer::PC1600) return;
    for (const BiquadSpec& spec : kPC1600Stages) {
        // RBJ Audio EQ Cookbook high-/low-pass, normalized by a0.
        const double w = 2.0 * kPi * spec.hz / m_sampleRate;
        const double alpha = std::sin(w) / (2.0 * spec.q);
        const double c = std::cos(w);
        const double a0 = 1.0 + alpha;
        Biquad& bq = m_stages[m_stageCount++];
        bq = Biquad{};
        const double edge = spec.highPass ? (1.0 + c) / 2.0 : (1.0 - c) / 2.0;
        bq.b0 = edge / a0;
        bq.b1 = (spec.highPass ? -2.0 * edge : 2.0 * edge) / a0;
        bq.b2 = edge / a0;
        bq.a1 = -2.0 * c / a0;
        bq.a2 = (1.0 - alpha) / a0;
    }
    m_gain = kPC1600Gain;
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
