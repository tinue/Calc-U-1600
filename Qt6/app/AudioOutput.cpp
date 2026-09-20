#include "AudioOutput.hpp"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QDebug>
#include <QIODevice>
#include <QMediaDevices>
#include <QTimer>
#include <algorithm>
#include <cstdlib>

#include "Audio/PiezoSampler.hpp"
#include "MachineController.hpp"

namespace {
constexpr int kSampleRate = PiezoSampler::kDefaultSampleRate;
constexpr int kBytesPerSample = 2;
// Sink buffer: comfortably more than one frame tick of audio, so a late
// tick doesn't underrun, without adding noticeable delay to a BEEP.
constexpr int kSinkBufferMs = 100;
// Silence written ahead of the first sound when the sink starts, so the
// jitter of 16 ms frame ticks lands inside the buffer instead of as gaps.
constexpr int kPrimeMs = 40;
constexpr int kIdleStopSamples = kSampleRate; // ~1 s of silence
// Audio the sink had no room for is carried to the next tick (the device
// consumes in hardware-period chunks, so bytesFree() is often briefly
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
    return std::all_of(samples, samples + n,
                       [](std::int16_t s) { return std::abs(s) <= kSilenceThreshold; });
}
} // namespace

AudioOutput::AudioOutput(QObject* parent)
    : QObject(parent), m_buffer(static_cast<std::size_t>(kSampleRate)) {
    // The first sink start in a process initializes the platform audio
    // backend, which blocks for ~0.5 s on macOS; later starts take a few
    // ms. Pay that once, right after startup, instead of stalling the
    // emulation at the first BEEP.
    QTimer::singleShot(0, this, [this] {
        if (startSink()) stopSink();
    });
}

AudioOutput::~AudioOutput() { stopSink(); }

void AudioOutput::pump(MachineController& controller, bool discard) {
    const std::size_t n = controller.drainAudio(m_buffer.data(), m_buffer.size());
    if (discard) {
        m_pending.clear();
        return;
    }
    if (n == 0 && m_pending.empty()) return;

    const bool silent = isSilent(m_buffer.data(), n);
    if (!m_io) {
        if (silent || !startSink()) return;
    }

    m_pending.insert(m_pending.end(), m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(n));
    const std::size_t room = static_cast<std::size_t>(std::max<qsizetype>(m_sink->bytesFree(), 0)) / kBytesPerSample;
    const std::size_t count = std::min(room, m_pending.size());
    if (count > 0) {
        m_io->write(reinterpret_cast<const char*>(m_pending.data()),
                    static_cast<qint64>(count) * kBytesPerSample);
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(count));
    }
    if (m_pending.size() > kMaxPendingSamples) {
        m_pending.erase(m_pending.begin(),
                        m_pending.end() - static_cast<std::ptrdiff_t>(kTrimPendingSamples));
    }

    m_silentSamples = silent ? m_silentSamples + static_cast<int>(n) : 0;
    if (m_silentSamples >= kIdleStopSamples) stopSink();
}

bool AudioOutput::startSink() {
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        if (!m_warned) qWarning() << "AudioOutput: no default audio output; buzzer is silent";
        m_warned = true;
        return false;
    }
    QAudioFormat format;
    format.setSampleRate(kSampleRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    m_sink = new QAudioSink(device, format, this);
    m_sink->setBufferSize(kSampleRate * kBytesPerSample * kSinkBufferMs / 1000);
    m_io = m_sink->start();
    if (!m_io || m_sink->error() != QAudio::NoError) {
        if (!m_warned) {
            qWarning() << "AudioOutput: could not open" << device.description()
                       << "- error" << m_sink->error() << "; buzzer is silent";
        }
        m_warned = true;
        stopSink();
        return false;
    }
    const std::vector<std::int16_t> prime(static_cast<std::size_t>(kSampleRate * kPrimeMs / 1000), 0);
    m_io->write(reinterpret_cast<const char*>(prime.data()),
                static_cast<qint64>(prime.size()) * kBytesPerSample);
    m_silentSamples = 0;
    return true;
}

void AudioOutput::stopSink() {
    if (!m_sink) return;
    m_sink->stop();
    m_sink->deleteLater();
    m_sink = nullptr;
    m_io = nullptr;
    m_silentSamples = 0;
    m_pending.clear();
}
