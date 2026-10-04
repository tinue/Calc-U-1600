#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../FileIO.hpp"

// Minimal RIFF/WAVE writer for PiezoSampler and TapeDeck output (mono,
// 16-bit PCM) -- used by the headless CLIs' --wav flag to capture buzzer
// audio for offline inspection, and for cassette recordings.
inline bool writeWavMono16(const std::string& path, const std::vector<int16_t>& samples, int sampleRate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto u32 = [f](uint32_t v) {
        const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        std::fwrite(b, 1, 4, f);
    };
    auto u16 = [f](uint16_t v) {
        const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
        std::fwrite(b, 1, 2, f);
    };
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * 2);
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);                                     // fmt chunk size
    u16(1);                                      // PCM
    u16(1);                                      // mono
    u32(static_cast<uint32_t>(sampleRate));
    u32(static_cast<uint32_t>(sampleRate) * 2);  // byte rate
    u16(2);                                      // block align
    u16(16);                                     // bits per sample
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    std::fwrite(samples.data(), 2, samples.size(), f); // little-endian host, as readWav assumes
    return std::fclose(f) == 0;
}

// Decoded WAV contents: the channels mixed down to mono, each sample
// scaled to -1..+1.
struct WavData {
    int sampleRate = 0;
    std::vector<float> samples;
};

// Minimal RIFF/WAVE reader for cassette playback: integer PCM of 8, 16, 24
// or 32 bits and 32-bit float, any rate and channel count (also
// WAVE_FORMAT_EXTENSIBLE with a PCM or float subformat). Returns false
// with a reason in `error` for anything else.
inline bool readWav(const std::string& path, WavData& out, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!readWholeFile(path, &bytes)) { error = "cannot open " + path; return false; }

    auto u16 = [&](size_t at) { return uint16_t(bytes[at] | bytes[at + 1] << 8); };
    auto u32 = [&](size_t at) { return uint32_t(u16(at)) | uint32_t(u16(at + 2)) << 16; };
    if (bytes.size() < 12 || std::memcmp(&bytes[0], "RIFF", 4) != 0 || std::memcmp(&bytes[8], "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file";
        return false;
    }
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    size_t dataAt = 0, dataSize = 0;
    for (size_t at = 12; at + 8 <= bytes.size();) {
        const uint32_t size = u32(at + 4);
        const size_t body = at + 8;
        if (std::memcmp(&bytes[at], "fmt ", 4) == 0 && size >= 16 && body + size <= bytes.size()) {
            format = u16(body);
            channels = u16(body + 2);
            rate = u32(body + 4);
            bits = u16(body + 14);
            if (format == 0xFFFE && size >= 26) format = u16(body + 24); // subformat GUID's first two bytes
        } else if (std::memcmp(&bytes[at], "data", 4) == 0) {
            dataAt = body;
            dataSize = std::min<size_t>(size, bytes.size() - body); // tolerate a truncated last chunk
        }
        at = body + size + (size & 1);
    }
    const bool isFloat = format == 3 && bits == 32;
    const bool isPcm = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    if (!isFloat && !isPcm) { error = "unsupported WAV encoding (PCM 8/16/24/32-bit or float 32-bit only)"; return false; }
    if (channels == 0 || rate == 0 || dataAt == 0) { error = "WAV file has no audio data"; return false; }

    const size_t frameBytes = size_t(channels) * (bits / 8);
    const size_t frames = dataSize / frameBytes;
    out.sampleRate = static_cast<int>(rate);
    out.samples.assign(frames, 0.0f);
    for (size_t i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (size_t c = 0; c < channels; ++c) {
            const uint8_t* p = &bytes[dataAt + i * frameBytes + c * (bits / 8)];
            float v;
            if (isFloat) {
                std::memcpy(&v, p, 4); // little-endian hosts only, like the writer
            } else if (bits == 8) {
                v = (float(p[0]) - 128.0f) / 128.0f; // 8-bit WAV is unsigned
            } else if (bits == 16) {
                v = float(int16_t(p[0] | p[1] << 8)) / 32768.0f;
            } else if (bits == 24) {
                const int32_t s = int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 8;
                v = float(s) / 8388608.0f;
            } else {
                v = float(int32_t(uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24)) / 2147483648.0f;
            }
            sum += v;
        }
        out.samples[i] = sum / float(channels);
    }
    return true;
}
