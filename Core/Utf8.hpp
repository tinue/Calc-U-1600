#pragma once
#include <cstddef>
#include <string>

// ── Minimal UTF-8 decoding for host text typed into a machine ────────────
//
// The typed-input paths (preset `type:` steps, Edit > Paste) walk host text
// one character at a time; a PC-1600 accented character (KBII, see
// PC1600TypedInput.hpp) arrives as a multi-byte UTF-8 sequence.

/// Decodes the character starting at `text[i]` into `cp` and advances `i`
/// past it. Returns false (and advances by one byte) for a malformed or
/// truncated sequence, so a caller can skip it and go on. Returns false
/// without advancing once `i` reaches the end.
inline bool decodeUtf8(const std::string& text, std::size_t& i, char32_t& cp) {
    if (i >= text.size()) return false;
    const auto lead = static_cast<unsigned char>(text[i]);
    int extra = 0;
    if (lead < 0x80) {
        cp = lead;
    } else if ((lead & 0xE0) == 0xC0) {
        cp = lead & 0x1F;
        extra = 1;
    } else if ((lead & 0xF0) == 0xE0) {
        cp = lead & 0x0F;
        extra = 2;
    } else if ((lead & 0xF8) == 0xF0) {
        cp = lead & 0x07;
        extra = 3;
    } else {
        ++i;
        return false;
    }
    if (i + static_cast<std::size_t>(extra) >= text.size()) { // truncated at the end
        ++i;
        return false;
    }
    for (int k = 1; k <= extra; ++k) {
        const auto b = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
        if ((b & 0xC0) != 0x80) {
            ++i;
            return false;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    i += static_cast<std::size_t>(extra) + 1;
    return true;
}
