#include "Shared/AudioPump.hpp"

#include <algorithm>
#include <cstdlib>

#include "Audio/PiezoSampler.hpp"
#include "Shared/MachineSession.hpp"

namespace shared {

namespace {
constexpr int kSampleRate = PiezoSampler::kDefaultSampleRate;
constexpr int kIdleStopSamples = kSampleRate; // ~1 s of silence
// Audio the sink had no room for is carried to the next tick (the device
// consumes in hardware-period chunks, so its room is often briefly
// short). Once a stall pushes that backlog past kMaxPendingSamples, the
// oldest is dropped back down to kTrimPendingSamples rather than just to
// the cap: production and consumption run at the same rate, so a backlog
// never drains by itself, and one sitting at the cap would glitch on
// every bit of tick jitter from then on.
constexpr std::size_t kMaxPendingSamples = kSampleRate * 50 / 1000;
constexpr std::size_t kTrimPendingSamples = kSampleRate * 15 / 1000;
// The DC blocker leaves a tiny tail after the line stops moving; anything
// this small is silence.
constexpr int kSilenceThreshold = 8;

bool isSilent(const std::int16_t* samples, std::size_t n) {
    return std::all_of(samples, samples + n, [](std::int16_t s) { return std::abs(s) <= kSilenceThreshold; });
}
} // namespace

AudioPump::AudioPump(AudioSink& sink, Warn warn)
    : m_sink(sink), m_warn(std::move(warn)), m_buffer(static_cast<std::size_t>(kSampleRate)) {}

AudioPump::~AudioPump() { stopSink(); }

void AudioPump::warmUp() {
    if (startSink()) stopSink();
}

void AudioPump::pump(MachineSession& session, bool discard) {
    const std::size_t n = session.drainAudio(m_buffer.data(), m_buffer.size());
    if (discard) {
        m_pending.clear();
        return;
    }
    feed(m_buffer.data(), n);
}

void AudioPump::feed(const std::int16_t* samples, std::size_t n) {
    if (n == 0 && m_pending.empty()) return;

    const bool silent = isSilent(samples, n);
    const bool justStarted = !m_open;
    if (justStarted) {
        if (silent || !startSink()) return;
    }

    // The sink went idle = it played out everything it had: the emulation
    // fell behind the audio clock and there's a gap in the sound. (A sink
    // started on this pump is idle only because it has no data yet.)
    if (!justStarted && m_sink.ranDry() && !silent) {
        m_warn("AudioOutput: sink ran dry (emulation behind audio clock) -- audible gap");
    }
    m_pending.insert(m_pending.end(), samples, samples + n);
    const std::size_t count = std::min(m_sink.room(), m_pending.size());
    if (count > 0) {
        m_sink.write(m_pending.data(), count);
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(count));
    }
    if (m_pending.size() > kMaxPendingSamples) {
        m_warn("AudioOutput: backlog " + std::to_string(m_pending.size() * 1000 / kSampleRate) +
               " ms beyond the sink buffer, dropping " +
               std::to_string((m_pending.size() - kTrimPendingSamples) * 1000 / kSampleRate) +
               " ms -- audible cut");
        m_pending.erase(m_pending.begin(), m_pending.end() - static_cast<std::ptrdiff_t>(kTrimPendingSamples));
    }

    m_silentSamples = silent ? m_silentSamples + static_cast<int>(n) : 0;
    if (m_silentSamples >= kIdleStopSamples) stopSink();
}

bool AudioPump::startSink() {
    if (!m_sink.start(kSampleRate, kSinkBufferMs)) return false;
    m_open = true;
    const std::vector<std::int16_t> prime(static_cast<std::size_t>(kSampleRate * kPrimeMs / 1000), 0);
    m_sink.write(prime.data(), prime.size());
    m_silentSamples = 0;
    return true;
}

void AudioPump::stopSink() {
    if (!m_open) return;
    m_sink.stop();
    m_open = false;
    m_silentSamples = 0;
    m_pending.clear();
}

} // namespace shared
