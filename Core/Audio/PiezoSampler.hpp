#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// ── Piezo buzzer drive line -> PCM ───────────────────────────────────────
//
// Both machines sound their buzzer by bit-banging one output line from a
// ROM delay loop (PC-1500: LH5811 OPC bit 6 / PC6; PC-1600: SC-7852 OPC
// port 18H bit 7, gated by bit 6 -- see the respective memory classes).
// Nothing here knows about BEEP: the owning memory class reports every
// change of that line via setLevel(), the owning machine credits elapsed
// CPU time via advance(), and this class turns the resulting 1-bit
// waveform into mono 16-bit PCM at a fixed host sample rate. Pitch and
// duration therefore come entirely from the emulated ROM's own cycle
// timing.
//
// Each output sample is the box-filtered average of the line over its
// sample interval (edges land between sample boundaries -- averaging keeps
// the duty cycle exact and suppresses the worst aliasing), followed by a
// one-pole DC-blocking high-pass: a real piezo is AC-coupled, so a line
// parked high or low (the ROM's idle / BEEP OFF levels) decays to silence
// without a click.
//
// Optionally a Transducer stage follows: a biquad cascade modelling how
// the machine's piezo disc actually sounds. The drive signal is still the
// ROM's own square wave; this only shapes it the way the buzzer's acoustic
// response does. Without it a low BEEP comes out as a raw square wave
// whose fundamental dominates. On a real unit the fundamental is almost
// inaudible and the ear hears the 1.5-4.5 kHz harmonics, so the pitch
// seems several times higher.
//
// Not thread-safe on its own -- the owning machine serializes access
// under its own mutex (step()/runCycles() on one side, drainAudio() on the
// other).
class PiezoSampler {
public:
    static constexpr int kDefaultSampleRate = 48000;

    enum class Transducer {
        None,   // the drive line as-is (after the DC blocker)
        PC1500, // PC-1500/1500A buzzer, fitted to a real-unit sweep (see .cpp)
        PC1600, // PC-1600 buzzer, fitted to real-unit recordings (see .cpp)
    };

    explicit PiezoSampler(double cpuHz, Transducer transducer = Transducer::None,
                          int sampleRate = kDefaultSampleRate);

    /// Switches the acoustic model on or off. Tests that measure the drive
    /// signal's pitch/timing turn it off; the host hears it on.
    void setTransducer(Transducer transducer);

    int sampleRate() const { return m_sampleRate; }

    /// Current drive-line level. Takes effect from the next advance().
    void setLevel(bool high) {
        if (high != m_level) ++m_edges;
        m_level = high;
    }
    bool level() const { return m_level; }
    /// Level changes seen so far -- lets a caller tell "the ROM is sounding
    /// the buzzer" apart from an idle loop without decoding any audio.
    uint64_t edgeCount() const { return m_edges; }

    /// Credit `cycles` CPU cycles of elapsed emulated time at the current
    /// level, emitting every sample whose interval completes.
    void advance(uint32_t cycles) {
        double remaining = static_cast<double>(cycles);
        while (m_phase + remaining >= m_cyclesPerSample) {
            const double take = m_cyclesPerSample - m_phase;
            if (m_level) m_area += take;
            remaining -= take;
            emitSample();
        }
        m_phase += remaining;
        if (m_level) m_area += remaining;
    }

    /// Moves up to `max` of the oldest buffered samples into `out` and
    /// returns how many were written.
    size_t drain(int16_t* out, size_t max);
    /// Buffered-sample count (what drain() could return right now).
    size_t available() const { return m_count; }
    /// Throws away everything buffered (fast-forward, model switch).
    void discard() { m_head = 0; m_count = 0; }

private:
    // Transposed direct form II; coefficients normalized so a0 = 1.
    struct Biquad {
        double b0{1}, b1{0}, b2{0}, a1{0}, a2{0};
        double z1{0}, z2{0};
        double process(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    static constexpr int kMaxStages = 3;

    void emitSample();

    double m_cyclesPerSample;
    int    m_sampleRate;
    double m_hpCoeff;      // DC blocker pole: y = x - x1 + R*y1

    bool   m_level{false};
    uint64_t m_edges{0};
    double m_phase{0.0};   // cycles into the current sample interval
    double m_area{0.0};    // cycles of that interval spent high
    double m_hpPrevIn{0.0};
    double m_hpPrevOut{0.0};

    Biquad m_stages[kMaxStages];
    int    m_stageCount{0};
    double m_gain{0.0};

    // Fixed-capacity ring (~1 s): the oldest samples are dropped when a
    // consumer isn't draining (headless runs, a stalled UI).
    std::vector<int16_t> m_ring;
    size_t m_head{0};      // index of the oldest sample
    size_t m_count{0};
};
