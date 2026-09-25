#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The LH5803's private 16KB internal ROM at C000-FFFF
// (PC1600-LH5803-C000-FFFF-new.bin), shared by LH5803Memory and
// LH5803SharedMemory. Reads as open bus (0xFF) until loaded.
class LH5803Rom {
public:
    static constexpr uint16_t kBase = 0xC000;
    static constexpr size_t   kSize = 0x4000; // 16384B

    /// Returns false (untouched) if `size` isn't exactly kSize bytes.
    bool load(const uint8_t* data, size_t size) {
        if (size != kSize) return false;
        std::memcpy(m_bytes.data(), data, kSize);
        m_loaded = true;
        return true;
    }

    /// Returns false (untouched) unless the file is exactly kSize bytes.
    bool loadFile(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::vector<uint8_t> buf(kSize + 1);
        size_t n = std::fread(buf.data(), 1, buf.size(), f);
        std::fclose(f);
        if (n != kSize) return false;
        return load(buf.data(), kSize);
    }

    /// `addr` must be >= kBase.
    uint8_t read(uint16_t addr) const { return m_loaded ? m_bytes[addr - kBase] : 0xFF; }

private:
    std::array<uint8_t, kSize> m_bytes{};
    bool m_loaded{false};
};
