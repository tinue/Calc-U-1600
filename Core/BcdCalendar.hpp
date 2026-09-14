#pragma once
#include <cstdint>

// Shared packed-BCD calendar helpers used by both real-time-clock models
// (PC-1500's uPD1990AC and the PC-1600 sub-CPU's own clock).

inline uint8_t bcdPack(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
inline int bcdUnpack(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }

inline bool bcdIsLeapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

inline int bcdDaysInMonth(int month, int year) {
    static const int len[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 31;
    if (month == 2 && bcdIsLeapYear(year)) return 29;
    return len[month - 1];
}

// Advances one packed-BCD calendar field. Returns true -- and wraps the
// field to `wrapTo` -- when it was already at `last`, i.e. the caller must
// carry into the next-coarser field.
inline bool bcdBumpField(uint8_t& field, int last, int wrapTo) {
    int n = bcdUnpack(field);
    if (n < last) {
        field = bcdPack(n + 1);
        return false;
    }
    field = bcdPack(wrapTo);
    return true;
}
