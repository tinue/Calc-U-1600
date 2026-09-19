#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Minimal RIFF/WAVE writer for PiezoSampler output (mono, 16-bit PCM) --
// used by the headless CLIs' --wav flag to capture buzzer audio for
// offline inspection.
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
    for (int16_t s : samples) u16(static_cast<uint16_t>(s));
    return std::fclose(f) == 0;
}
