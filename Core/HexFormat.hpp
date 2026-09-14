#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

// Formats a byte as "0xXX" for error messages and diagnostics.
inline std::string hex2(uint8_t b) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%02X", b);
    return buf;
}
