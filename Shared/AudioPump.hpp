#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace shared {

class MachineSession;

// The host's audio output, push mode, mono int16 at the buzzer's sample
// rate (Qt: QAudioSink; a browser: an AudioWorklet's ring buffer).
class AudioSink {
public:
    virtual ~AudioSink() = default;
    // Opens the default output with a buffer of `bufferMs`; false if there
    // is none or it can't be opened (the sink reports why, once).
    virtual bool start(int sampleRate, int bufferMs) = 0;
    virtual void stop() = 0;
    // Samples the sink can take right now.
    virtual std::size_t room() const = 0;
    virtual void write(const std::int16_t* samples, std::size_t count) = 0;
    // True when the sink played out everything it had (an audible gap).
    virtual bool ranDry() const = 0;
};

// ── AudioPump: the buzzer's PCM from the session to the sink ────────────
//
// Driven from the frame tick: pump() drains whatever the emulation
// produced since the last tick and writes as much as the sink has room
// for; the remainder carries over, up to a small cap, so latency can't
// build up. The sink is open only while there is something to hear:
// started on the first non-silent chunk (primed with a little silence so
// tick jitter lands inside its buffer) and stopped after ~1 s of silence,
// so an idle emulator doesn't keep the audio device open.
class AudioPump {
public:
    using Warn = std::function<void(const std::string&)>;
    AudioPump(AudioSink& sink, Warn warn);
    ~AudioPump();

    // `discard`: throw this tick's audio away instead of playing it (turbo
    // fast-forward, scripted runs -- not real time).
    void pump(MachineSession& session, bool discard);
    // pump()'s second half: plays `n` freshly drained samples.
    void feed(const std::int16_t* samples, std::size_t n);
    // Starts and stops the sink once. The first sink start in a process
    // initializes the platform audio backend, which can block (~0.5 s on
    // macOS); call this right after startup instead of paying it at the
    // first BEEP.
    void warmUp();

    static constexpr int kSinkBufferMs = 100;
    static constexpr int kPrimeMs = 40;

private:
    bool startSink();
    void stopSink();

    AudioSink& m_sink;
    Warn m_warn;
    bool m_open = false;
    std::vector<std::int16_t> m_buffer;   // drain scratch, one second's worth
    std::vector<std::int16_t> m_pending;  // drained but not yet accepted by the sink
    int m_silentSamples = 0;              // consecutive silent samples while playing
};

} // namespace shared
