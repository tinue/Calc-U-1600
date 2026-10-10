#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ── Hex dumps for the inspector's Dump menu ──────────────────────────────
//
// 16 bytes a row, an extra space after the eighth, then the printable ASCII
// (20H-7EH, '.' otherwise). Runs of rows made of 00H/FFH only fold into one
// "...." line, so a mostly empty area stays short; the first and last row
// are always shown, so the dump's bounds are visible.
//
//   C0C5: 00 0A 0B DE 22 48 49 22  0D FF 00 00 00 00 00 00  ...."HI"........
namespace inspect {

/// `n` bytes from `data`, labelled from address `base`. `addrDigits` is 4
/// for a CPU address, 5 or 6 for an offset into a large module.
std::vector<std::string> hexDump(const uint8_t* data, size_t n, uint32_t base, int addrDigits = 4);

inline std::vector<std::string> hexDump(const std::vector<uint8_t>& bytes, uint32_t base, int addrDigits = 4) {
    return hexDump(bytes.data(), bytes.size(), base, addrDigits);
}

/// A size as the views print it: "16K" for whole kilobytes, "197 B" else.
std::string sizeLabel(uint32_t bytes);

/// printf into a std::string.
std::string fmt(const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

}  // namespace inspect
