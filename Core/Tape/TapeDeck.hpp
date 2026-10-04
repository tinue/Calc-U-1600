#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "../Audio/WavFile.hpp"

// ── Cassette recorder (CE-152 class) backed by a WAV file ────────────────
//
// The recorder a CE-150 or CE-1600P drives through its MIC/EAR/REM jacks.
// It knows nothing about the machine or the tape format: the interface
// card reports the remote relay (setMotor) and the cassette-output line
// (setOutputLevel), credits elapsed CPU time (advance), and reads the
// conditioned cassette-input line (inputLevel). Bit timing and decoding
// are entirely the emulated ROM's own.
//
// Time is emulated time only: the tape moves by advance()'s CPU cycles
// while the motor runs, never by host time. So a recording or a playback
// is the same at real-time pacing, in turbo, or in a flat-out CLI run.
//
// Play: the WAV is turned into the input line's logic level once, at load
// time, the way the interface's input amplifier does it -- a DC-blocking
// high-pass, then a comparator with hysteresis that follows the signal's
// envelope. That reads 8-bit bin2wav files and real recordings alike, and
// silence (or hiss) leaves the line where it was rather than chattering.
//
// Record: the output line is box-filtered into 48 kHz 16-bit PCM (each
// sample the line's average over its interval, so edges between sample
// boundaries keep their exact duty cycle), through the same kind of
// DC blocker a recorder's AC-coupled input has. Only motor-on time is
// recorded, as on a real tape.
//
// Not thread-safe on its own -- the owning machine serializes access under
// its own mutex.
class TapeDeck {
public:
    static constexpr int kRecordSampleRate = 48000;

    enum class Mode { Empty, Play, Record };

    explicit TapeDeck(double cpuHz) : m_cpuHz(cpuHz) {}

    // ── Arming ───────────────────────────────────────────────────────────

    /// Inserts `path` for playback (CLOAD), rewound. False with a reason in
    /// `error` if the file can't be read; the deck is then left as it was.
    bool loadForPlay(const std::string& path, std::string& error) {
        WavData wav;
        if (!readWav(path, wav, error)) return false;
        loadForPlay(wav.samples, wav.sampleRate);
        m_path = path;
        return true;
    }

    /// Same, from samples in -1..+1 (tests).
    void loadForPlay(const std::vector<float>& samples, int sampleRate) {
        eject();
        m_recording.clear();
        m_mode = Mode::Play;
        m_rate = sampleRate;
        m_levels = conditionInput(samples, sampleRate);
    }

    /// Inserts a blank tape for recording (CSAVE). `path` is where eject()
    /// writes the WAV; empty keeps the recording in memory only (tests).
    void armRecord(const std::string& path) {
        eject();
        m_recording.clear();
        m_mode = Mode::Record;
        m_rate = kRecordSampleRate;
        m_path = path;
        m_cyclesPerSample = m_cpuHz / kRecordSampleRate;
    }

    /// Takes the tape out. A recording is written to its file first; false
    /// with a reason in `error` if that fails. The recorded samples stay
    /// readable through recording() until the next arming.
    bool eject(std::string* error = nullptr) {
        bool ok = true;
        if (m_mode == Mode::Record && !m_path.empty() && !writeWavMono16(m_path, m_recording, m_rate)) {
            ok = false;
            if (error) *error = "cannot write " + m_path;
        }
        m_mode = Mode::Empty;
        m_path.clear();
        m_levels.clear();
        m_posCycles = 0;
        m_phase = 0.0;
        m_area = 0.0;
        m_hpIn = 0.0;
        m_hpOut = 0.0;
        return ok;
    }

    Mode mode() const { return m_mode; }
    const std::string& path() const { return m_path; }
    const std::vector<int16_t>& recording() const { return m_recording; }

    // ── Signals from the interface ───────────────────────────────────────

    /// Remote relay: the tape moves only while it is closed.
    void setMotor(bool on) { m_motor = on; }
    bool motorOn() const { return m_motor; }

    /// Cassette-output line (MIC). Takes effect from the next advance().
    void setOutputLevel(bool high) { m_out = high; }

    /// Cassette-input line (EAR) after the input amplifier, at the current
    /// tape position. Holds its last level when nothing is playing.
    bool inputLevel() const {
        if (m_mode != Mode::Play || m_levels.empty()) return m_lastIn;
        const uint64_t i = sampleIndex();
        m_lastIn = i < m_levels.size() ? m_levels[i] != 0 : m_levels.back() != 0;
        return m_lastIn;
    }

    /// Credits `cycles` CPU cycles of emulated time.
    void advance(uint64_t cycles) {
        if (!m_motor || m_mode == Mode::Empty) return;
        m_posCycles += cycles;
        if (m_mode == Mode::Record) record(static_cast<double>(cycles));
    }

    // ── Tape counter ─────────────────────────────────────────────────────

    double positionSeconds() const { return m_posCycles / m_cpuHz; }
    /// Playback length; for a recording, how much has been recorded.
    double lengthSeconds() const {
        if (m_mode == Mode::Play && m_rate > 0) return double(m_levels.size()) / m_rate;
        return positionSeconds();
    }
    bool atEnd() const { return m_mode == Mode::Play && sampleIndex() >= m_levels.size(); }

private:
    double m_cpuHz;
    Mode m_mode = Mode::Empty;
    std::string m_path;
    int m_rate = 0;
    bool m_motor = false;
    uint64_t m_posCycles = 0; // motor-on CPU cycles since the tape was inserted

    // Play
    std::vector<uint8_t> m_levels; // the input line's level per WAV sample
    mutable bool m_lastIn = false;

    // Record
    bool m_out = false;
    double m_cyclesPerSample = 0.0;
    double m_phase = 0.0; // cycles into the current output sample
    double m_area = 0.0;  // cycles of it spent high
    double m_hpIn = 0.0, m_hpOut = 0.0;
    std::vector<int16_t> m_recording;

    // Corner of both DC blockers: far below the 1-3 kHz tape tones, so a
    // tone's half-cycle droops by only a few percent.
    static constexpr double kHighPassHz = 20.0;
    // Recording level, about -3 dBFS for a full-swing square wave.
    static constexpr double kRecordGain = 0.7 * 32767.0;

    uint64_t sampleIndex() const {
        return static_cast<uint64_t>(static_cast<long double>(m_posCycles) * m_rate / m_cpuHz);
    }

    static double highPassCoefficient(int rate) {
        return 1.0 - 2.0 * 3.14159265358979323846 * kHighPassHz / rate;
    }

    void record(double cycles) {
        if (m_recording.empty() && m_phase == 0.0) m_recording.reserve(size_t(m_rate) * 60);
        const double r = highPassCoefficient(m_rate);
        while (m_phase + cycles >= m_cyclesPerSample) {
            const double take = m_cyclesPerSample - m_phase;
            if (m_out) m_area += take;
            cycles -= take;
            const double x = 2.0 * m_area / m_cyclesPerSample - 1.0; // -1..+1
            m_hpOut = x - m_hpIn + r * m_hpOut;
            m_hpIn = x;
            const double s = std::min(32767.0, std::max(-32768.0, m_hpOut * kRecordGain));
            m_recording.push_back(static_cast<int16_t>(std::lround(s)));
            m_phase = 0.0;
            m_area = 0.0;
        }
        m_phase += cycles;
        if (m_out) m_area += cycles;
    }

    // The input amplifier: DC blocker, then a comparator whose hysteresis
    // is a quarter of the recent peak level (decaying over ~20 ms), never
    // below kFloor so a quiet noise floor can't toggle it.
    static std::vector<uint8_t> conditionInput(const std::vector<float>& in, int rate) {
        constexpr double kFloor = 0.02;
        const double r = highPassCoefficient(rate);
        const double decay = std::exp(-1.0 / (0.020 * rate));
        std::vector<uint8_t> out(in.size());
        double prevIn = in.empty() ? 0.0 : in.front(); // no step at the first sample
        double y = 0.0, env = 0.0;
        bool level = false;
        for (size_t i = 0; i < in.size(); ++i) {
            y = in[i] - prevIn + r * y;
            prevIn = in[i];
            env = std::max(std::fabs(y), env * decay);
            const double h = std::max(kFloor, 0.25 * env);
            if (y > h) level = true;
            else if (y < -h) level = false;
            out[i] = level ? 1 : 0;
        }
        return out;
    }
};
