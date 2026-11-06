#pragma once
#include <QObject>
#include <cstdint>
#include <memory>

#include "Shared/AudioPump.hpp"

class QAudioSink;
class QIODevice;
class MachineController;

// Plays the active machine's buzzer audio on the host's default audio
// output: shared::AudioPump decides what to write when (backlog, priming,
// idle stop -- Shared/AudioPump.hpp); this is its QAudioSink.
class AudioOutput : public QObject, private shared::AudioSink {
    Q_OBJECT
public:
    explicit AudioOutput(QObject* parent = nullptr);
    ~AudioOutput() override;

    // `discard`: throw this tick's audio away instead of playing it (turbo
    // fast-forward -- sped-up audio would just be noise).
    void pump(MachineController& controller, bool discard);

private:
    // shared::AudioSink
    bool start(int sampleRate, int bufferMs) override;
    void stop() override;
    std::size_t room() const override;
    void write(const std::int16_t* samples, std::size_t count) override;
    bool ranDry() const override;

    std::unique_ptr<shared::AudioPump> m_pump;
    QAudioSink* m_sink = nullptr;
    QIODevice* m_io = nullptr;  // m_sink's push device while playing
    bool m_warned = false;      // no device / open failure logged once
};
