#include "AudioOutput.hpp"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QDebug>
#include <QIODevice>
#include <QMediaDevices>
#include <QTimer>
#include <algorithm>

#include "MachineController.hpp"

namespace {
constexpr int kBytesPerSample = 2;
} // namespace

AudioOutput::AudioOutput(QObject* parent) : QObject(parent) {
    shared::AudioSink& sink = *this;
    m_pump = std::make_unique<shared::AudioPump>(
        sink, [](const std::string& text) { qWarning().noquote() << QString::fromStdString(text); });
    // Pay the audio backend's first-start cost right after startup.
    QTimer::singleShot(0, this, [this] { m_pump->warmUp(); });
}

AudioOutput::~AudioOutput() { m_pump.reset(); }

void AudioOutput::pump(MachineController& controller, bool discard) { m_pump->pump(controller.session(), discard); }

bool AudioOutput::start(int sampleRate, int bufferMs) {
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        if (!m_warned) qWarning() << "AudioOutput: no default audio output; buzzer is silent";
        m_warned = true;
        return false;
    }
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    m_sink = new QAudioSink(device, format, this);
    m_sink->setBufferSize(sampleRate * kBytesPerSample * bufferMs / 1000);
    m_io = m_sink->start();
    if (!m_io || m_sink->error() != QAudio::NoError) {
        if (!m_warned) {
            qWarning() << "AudioOutput: could not open" << device.description() << "- error" << m_sink->error()
                       << "; buzzer is silent";
        }
        m_warned = true;
        stop();
        return false;
    }
    return true;
}

void AudioOutput::stop() {
    if (!m_sink) return;
    m_sink->stop();
    m_sink->deleteLater();
    m_sink = nullptr;
    m_io = nullptr;
}

std::size_t AudioOutput::room() const {
    return m_sink ? static_cast<std::size_t>(std::max<qsizetype>(m_sink->bytesFree(), 0)) / kBytesPerSample : 0;
}

void AudioOutput::write(const std::int16_t* samples, std::size_t count) {
    if (m_io) m_io->write(reinterpret_cast<const char*>(samples), static_cast<qint64>(count) * kBytesPerSample);
}

bool AudioOutput::ranDry() const { return m_sink && m_sink->state() == QAudio::IdleState; }
