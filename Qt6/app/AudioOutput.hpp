#pragma once
#include <QObject>
#include <cstdint>
#include <vector>

class QAudioSink;
class QIODevice;
class MachineController;

// Plays the active machine's buzzer audio (Core's PiezoSampler PCM, drained
// through MachineController) on the host's default audio output. Push
// mode, driven from MainWindow's frame tick: pump() drains whatever the
// emulation produced since the last tick and writes as much as the sink
// has room for. The remainder carries over to the next tick, up to a
// small cap, so latency can't build up.
//
// The sink exists only while there is something to hear. It's created on
// the first non-silent chunk, against whatever the default output is at
// that moment, and stopped again after ~1 s of silence, so an idle
// emulator doesn't keep the audio device open.
class AudioOutput : public QObject {
    Q_OBJECT
public:
    explicit AudioOutput(QObject* parent = nullptr);
    ~AudioOutput();

    // `discard`: throw this tick's audio away instead of playing it (turbo
    // fast-forward -- sped-up audio would just be noise).
    void pump(MachineController& controller, bool discard);

private:
    bool startSink();
    void stopSink();

    QAudioSink* m_sink = nullptr;
    QIODevice* m_io = nullptr;          // m_sink's push device while playing
    std::vector<std::int16_t> m_buffer; // drain scratch, one second's worth
    std::vector<std::int16_t> m_pending; // drained but not yet accepted by the sink
    int m_silentSamples = 0;            // consecutive silent samples written while playing
    bool m_warned = false;              // no device / open failure logged once
};
