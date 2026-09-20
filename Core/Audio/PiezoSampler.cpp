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
} // namespace

PiezoSampler::PiezoSampler(double cpuHz, int sampleRate)
    : m_cyclesPerSample(cpuHz / sampleRate),
      m_sampleRate(sampleRate),
      m_hpCoeff(1.0 - 2.0 * kPi * kHighPassHz / sampleRate),
      m_ring(static_cast<size_t>(sampleRate)) {}

void PiezoSampler::emitSample() {
    const double x = m_area / m_cyclesPerSample; // 0..1 duty over the interval
    m_phase = 0.0;
    m_area = 0.0;
    const double y = x - m_hpPrevIn + m_hpCoeff * m_hpPrevOut;
    m_hpPrevIn = x;
    m_hpPrevOut = y;

    const double scaled = std::clamp(y * kGain, -1.0, 1.0) * 32767.0;
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
