#pragma once
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>

// Formats a byte as "0xXX" for error messages and diagnostics.
inline std::string hex2(uint8_t b) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%02X", b);
    return buf;
}

// Parses exactly `len` hex digits of `s` starting at `pos`. False if the
// field runs past the end or holds a non-hex character.
inline bool parseHexField(const std::string& s, size_t pos, size_t len, uint32_t* value) {
    if (pos + len > s.size()) return false;
    uint32_t v = 0;
    for (size_t i = pos; i < pos + len; i++) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (!std::isxdigit(c)) return false;
        v = v * 16 + static_cast<uint32_t>(std::isdigit(c) ? c - '0' : std::toupper(c) - 'A' + 10);
    }
    *value = v;
    return true;
}
