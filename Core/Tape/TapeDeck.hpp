#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
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
// boundaries keep their exact duty cycle), then AC-coupled at 12 Hz, as
// measured on a real PC-1600 + CE-1600P recorded through a Mac line input
// (2026-10-04: a lone step decays with a 13.8 ms time constant; the tones
// are flat-topped). So a parked line decays to silence, and each lone step
// of the line -- the ROM parking it for a gap, or switching it on before
// the motor -- records as a ~1.8x spike that dies away within ~30 ms, as
// on the real tape. The interfaces' low-pass and level aren't modelled.
// Only motor-on time is recorded, as on a real tape, and the WAV is saved
// (whole) each time the motor stops, so a finished CSAVE is on disk at once
// and the next CSAVE on the same tape appends.
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

    /// Inserts a blank tape for recording (CSAVE). The WAV goes to `path`
    /// each time the motor stops (see setMotor()); empty keeps the
    /// recording in memory only (tests).
    void armRecord(const std::string& path) {
        eject();
        m_recording.clear();
        m_savedSamples = 0;
        m_mode = Mode::Record;
        m_rate = kRecordSampleRate;
        m_path = path;
        m_cyclesPerSample = m_cpuHz / kRecordSampleRate;
    }

    /// Takes the tape out. A recording is normally saved already (at each
    /// motor stop); anything not yet saved -- Eject while the motor still
    /// runs -- is written first, false with a reason in `error` if that
    /// fails. The recorded samples stay readable through recording() until
    /// the next arming.
    bool eject(std::string* error = nullptr) {
        const bool ok = saveRecording(error);
        m_mode = Mode::Empty;
        m_path.clear();
        m_levels.clear();
        m_posCycles = 0;
        m_phase = 0.0;
        m_area = 0.0;
        m_hpIn = 0.0;
        m_hpOut = 0.0;
        m_priming.clear();
        return ok;
    }

    /// A snapshot for the GUI's tape counter.
    struct Status {
        Mode mode = Mode::Empty;
        bool motor = false;
        double position = 0.0; // seconds
        double length = 0.0;   // seconds; for a recording, recorded so far
        std::string path;
    };
    Status status() const { return {m_mode, m_motor, positionSeconds(), lengthSeconds(), m_path}; }

    /// The reason the last automatic save (at a motor stop) failed, once;
    /// empty if it didn't. The save happens inside emulation, where there
    /// is no one to return it to.
    std::string takeLastError() { return std::exchange(m_lastError, std::string()); }

    Mode mode() const { return m_mode; }
    const std::string& path() const { return m_path; }
    const std::vector<int16_t>& recording() const { return m_recording; }

    // ── Signals from the interface ───────────────────────────────────────

    /// Remote relay: the tape moves only while it is closed. When it opens
    /// on a recording, the WAV is saved (whole), so a finished CSAVE is on
    /// disk at once; a later CSAVE on the same tape appends and saves again.
    void setMotor(bool on) {
        if (m_motor && !on && m_mode == Mode::Record) {
            std::string error;
            if (!saveRecording(&error)) m_lastError = error;
        }
        m_motor = on;
    }
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
    size_t m_savedSamples = 0; // how much of m_recording is on disk
    std::string m_lastError;   // see takeLastError()

    // Writes the recording if it has samples not on disk yet; true if there
    // was nothing to do.
    bool saveRecording(std::string* error) {
        if (m_mode != Mode::Record || m_path.empty() || m_recording.size() == m_savedSamples) return true;
        if (!writeWavMono16(m_path, m_recording, m_rate)) {
            if (error) *error = "cannot write " + m_path;
            return false;
        }
        m_savedSamples = m_recording.size();
        return true;
    }

    // Playback input coupling: far below the 1-3 kHz tape tones, so it
    // follows any recording's DC drift without touching the tones.
    static constexpr double kPlayHighPassHz = 20.0;
    // Recording output coupling, see the class comment.
    static constexpr double kRecordHighPassHz = 12.0;
    // Recording level. A lone step of the line (or a tone's first edge
    // after a parked line) swings to 2x the tone's level, as on the real
    // tape; 0.45 keeps that below full scale (-1 dBFS).
    static constexpr double kRecordGain = 0.45 * 32767.0;

    uint64_t sampleIndex() const {
        return static_cast<uint64_t>(static_cast<long double>(m_posCycles) * m_rate / m_cpuHz);
    }

    static double highPassCoefficient(int rate, double hz) {
        return 1.0 - 2.0 * 3.14159265358979323846 * hz / rate;
    }

    // The line was at work long before the motor started, so the
    // recorder's coupling has settled on its average: its level if it's
    // parked, about 0 if it carries a tone (the PC-1500's leader). The
    // blocker starts there -- from 0 the tape would begin with a
    // full-scale click. The first kPrimeSamples (2 ms, several cycles of
    // the slowest tape tone) are held back to measure that average.
    static constexpr size_t kPrimeSamples = kRecordSampleRate / 500;
    std::vector<double> m_priming;

    void pushSample(double x, double r) {
        if (m_recording.empty() && m_priming.size() < kPrimeSamples) {
            m_priming.push_back(x);
            if (m_priming.size() < kPrimeSamples) return;
            double mean = 0.0;
            for (double v : m_priming) mean += v;
            mean /= double(m_priming.size());
            m_hpIn = m_priming.front();
            m_hpOut = m_priming.front() - mean;
            std::vector<double> primed;
            primed.swap(m_priming);
            emitSample(m_hpOut);
            for (size_t i = 1; i < primed.size(); ++i) filterAndEmit(primed[i], r);
            return;
        }
        filterAndEmit(x, r);
    }
    void filterAndEmit(double x, double r) {
        m_hpOut = x - m_hpIn + r * m_hpOut;
        m_hpIn = x;
        emitSample(m_hpOut);
    }
    void emitSample(double y) {
        const double s = std::min(32767.0, std::max(-32768.0, y * kRecordGain));
        m_recording.push_back(static_cast<int16_t>(std::lround(s)));
    }

    void record(double cycles) {
        if (m_recording.empty() && m_phase == 0.0) m_recording.reserve(size_t(m_rate) * 60);
        const double r = highPassCoefficient(m_rate, kRecordHighPassHz);
        while (m_phase + cycles >= m_cyclesPerSample) {
            const double take = m_cyclesPerSample - m_phase;
            if (m_out) m_area += take;
            cycles -= take;
            pushSample(2.0 * m_area / m_cyclesPerSample - 1.0, r); // -1..+1
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
        const double r = highPassCoefficient(rate, kPlayHighPassHz);
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
