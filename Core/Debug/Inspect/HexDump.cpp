#include "HexDump.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace inspect {

std::string fmt(const char* format, ...) {
    char buf[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return buf;
}

std::string sizeLabel(uint32_t bytes) {
    return bytes != 0 && bytes % 1024 == 0 ? fmt("%uK", bytes / 1024) : fmt("%u B", bytes);
}

std::vector<std::string> hexDump(const uint8_t* data, size_t n, uint32_t base, int addrDigits) {
    std::vector<std::string> out;
    const size_t rows = (n + 15) / 16;
    size_t skipFrom = SIZE_MAX; // first row of the current filler run
    auto flush = [&](size_t row) {
        if (skipFrom == SIZE_MAX) return;
        out.push_back(fmt("%0*X–%0*X: (00/FF only)", addrDigits, unsigned(base + skipFrom * 16), addrDigits,
                          unsigned(base + row * 16 - 1)));
        skipFrom = SIZE_MAX;
    };
    for (size_t row = 0; row < rows; ++row) {
        const size_t start = row * 16;
        const size_t end = std::min(start + 16, n);
        bool filler = true;
        for (size_t i = start; i < end; ++i)
            if (data[i] != 0x00 && data[i] != 0xFF) { filler = false; break; }
        if (filler && row != 0 && row + 1 != rows) {
            if (skipFrom == SIZE_MAX) skipFrom = row;
            continue;
        }
        flush(row);
        // The hex digits by table: a big RAM disk dumps tens of thousands of
        // rows under the machine lock.
        static const char kHex[] = "0123456789ABCDEF";
        std::string line = fmt("%0*X: ", addrDigits, unsigned(base + start));
        std::string ascii;
        for (size_t i = start; i < start + 16; ++i) {
            if (i - start == 8) line += ' ';
            if (i < end) {
                line += kHex[data[i] >> 4];
                line += kHex[data[i] & 0x0F];
                line += ' ';
                ascii += (data[i] >= 0x20 && data[i] < 0x7F) ? char(data[i]) : '.';
            } else {
                line += "   ";
            }
        }
        out.push_back(line + " " + ascii);
    }
    return out;
}

}  // namespace inspect
