#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Minimal, dependency-free PNG encoder for 8-bit greyscale images (header-
// only, like Core/Audio/WavFile.hpp) -- enough for the LCD screenshot
// (Core/Display/LcdScreenshot) without pulling zlib into the Qt-free CLIs.
//
// Compression is a single fixed-Huffman deflate block whose only matches
// are runs: distance 1 (horizontal runs, e.g. the white background) and
// distance = one scanline (a row repeating the row above). That's all an
// LCD dot-matrix image needs to shrink from ~1 MB raw to a few KB. Every
// scanline uses filter type 0 (None). A pHYs chunk records the physical
// resolution so viewers open the file at its true size.

namespace png_detail {

inline uint32_t crc32(const uint8_t* data, std::size_t len, uint32_t crc = 0) {
    static const auto table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

inline uint32_t adler32(const std::vector<uint8_t>& data) {
    uint32_t a = 1, b = 0;
    for (uint8_t byte : data) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

class BitWriter {
public:
    std::vector<uint8_t> bytes;
    // `count` bits of `value`, least-significant bit first (deflate's
    // order for extra bits and header fields).
    void bits(uint32_t value, int count) {
        for (int i = 0; i < count; ++i) {
            if (m_bitPos == 0) bytes.push_back(0);
            if ((value >> i) & 1u) bytes.back() |= static_cast<uint8_t>(1u << m_bitPos);
            m_bitPos = (m_bitPos + 1) & 7;
        }
    }
    // A Huffman code: packed most-significant bit first.
    void code(uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) bits((value >> i) & 1u, 1);
    }

private:
    int m_bitPos = 0;
};

inline void fixedLiteral(BitWriter& w, int sym) {
    if (sym <= 143) w.code(0x30 + sym, 8);
    else if (sym <= 255) w.code(0x190 + (sym - 144), 9);
    else if (sym <= 279) w.code(sym - 256, 7);
    else w.code(0xC0 + (sym - 280), 8);
}

inline void fixedMatch(BitWriter& w, int length, int distance) {
    static const int kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                     35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                      3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const int kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                      193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                      6145, 8193, 12289, 16385, 24577};
    static const int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    int li = 28;
    while (kLenBase[li] > length) --li;
    fixedLiteral(w, 257 + li);
    w.bits(static_cast<uint32_t>(length - kLenBase[li]), kLenExtra[li]);
    int di = 29;
    while (kDistBase[di] > distance) --di;
    w.code(static_cast<uint32_t>(di), 5);
    w.bits(static_cast<uint32_t>(distance - kDistBase[di]), kDistExtra[di]);
}

// zlib stream (RFC 1950) around one fixed-Huffman deflate block (RFC 1951).
inline std::vector<uint8_t> zlibCompress(const std::vector<uint8_t>& data, int rowStride) {
    BitWriter w;
    w.bytes = {0x78, 0x01};
    w.bits(1, 1); // BFINAL
    w.bits(1, 2); // BTYPE = 01, fixed Huffman
    const std::size_t n = data.size();
    std::size_t i = 0;
    while (i < n) {
        int bestLen = 0, bestDist = 0;
        for (int dist : {1, rowStride}) {
            if (dist <= 0 || dist > 32768 || i < static_cast<std::size_t>(dist)) continue;
            int len = 0;
            while (len < 258 && i + len < n && data[i + len] == data[i + len - dist]) ++len;
            if (len > bestLen) { bestLen = len; bestDist = dist; }
        }
        if (bestLen >= 3) {
            fixedMatch(w, bestLen, bestDist);
            i += static_cast<std::size_t>(bestLen);
        } else {
            fixedLiteral(w, data[i]);
            ++i;
        }
    }
    fixedLiteral(w, 256); // end of block
    const uint32_t adler = adler32(data);
    for (int shift = 24; shift >= 0; shift -= 8) w.bytes.push_back(static_cast<uint8_t>(adler >> shift));
    return w.bytes;
}

inline void putBE32(std::vector<uint8_t>& out, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
}

inline void putChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& payload) {
    putBE32(out, static_cast<uint32_t>(payload.size()));
    const std::size_t typeAt = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    putBE32(out, crc32(out.data() + typeAt, payload.size() + 4));
}

} // namespace png_detail

/// Encodes a width x height 8-bit greyscale image (`pixels` row-major,
/// 0 = black, 255 = white) as a PNG file image in memory.
/// `pixelsPerMeter` goes into pHYs (0 = omit the chunk).
inline std::vector<uint8_t> encodePngGray8(const std::vector<uint8_t>& pixels, int width, int height,
                                           uint32_t pixelsPerMeter) {
    using namespace png_detail;
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(width + 1) * height);
    for (int y = 0; y < height; ++y) {
        raw.push_back(0); // filter: None
        const auto row = pixels.begin() + static_cast<std::ptrdiff_t>(y) * width;
        raw.insert(raw.end(), row, row + width);
    }

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    putBE32(ihdr, static_cast<uint32_t>(width));
    putBE32(ihdr, static_cast<uint32_t>(height));
    ihdr.insert(ihdr.end(), {8, 0, 0, 0, 0}); // 8-bit, greyscale, deflate, filter 0, no interlace
    putChunk(out, "IHDR", ihdr);
    if (pixelsPerMeter > 0) {
        std::vector<uint8_t> phys;
        putBE32(phys, pixelsPerMeter);
        putBE32(phys, pixelsPerMeter);
        phys.push_back(1); // unit: metre
        putChunk(out, "pHYs", phys);
    }
    putChunk(out, "IDAT", zlibCompress(raw, width + 1));
    putChunk(out, "IEND", {});
    return out;
}

/// Writes encodePngGray8()'s output to `path`. False (with `error` set)
/// if the file can't be written.
inline bool writePngGray8(const std::string& path, const std::vector<uint8_t>& pixels, int width, int height,
                          uint32_t pixelsPerMeter, std::string* error) {
    const std::vector<uint8_t> png = encodePngGray8(pixels, width, height, pixelsPerMeter);
    std::FILE* fh = std::fopen(path.c_str(), "wb");
    if (!fh) {
        if (error) *error = "could not open " + path + " for writing";
        return false;
    }
    const bool ok = std::fwrite(png.data(), 1, png.size(), fh) == png.size();
    if (std::fclose(fh) != 0 || !ok) {
        if (error) *error = "could not write " + path;
        return false;
    }
    return true;
}
