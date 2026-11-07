#pragma once
#include <QObject>
#include <cstdint>

#include "Shared/AudioPump.hpp"

class QAudioSink;
class QIODevice;

// The host's default audio output as a shared::AudioSink (a QAudioSink):
// the session's AudioPump (AppSession::attachAudio()) decides what to
// write when -- backlog, priming, idle stop, turbo (Shared/AudioPump.hpp).
// Must outlive the session.
class AudioOutput : public QObject, private shared::AudioSink {
    Q_OBJECT
public:
    explicit AudioOutput(QObject* parent = nullptr);

    shared::AudioSink& sink() { return *this; }

private:
    // shared::AudioSink
    bool start(int sampleRate, int bufferMs) override;
    void stop() override;
    std::size_t room() const override;
    void write(const std::int16_t* samples, std::size_t count) override;
    bool ranDry() const override;

    QAudioSink* m_sink = nullptr;
    QIODevice* m_io = nullptr;  // m_sink's push device while playing
    bool m_warned = false;      // no device / open failure logged once
};
