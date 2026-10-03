#pragma once
#include <cstdint>
#include <optional>
#include <utility>

#include "../Display/LcdCharsets.hpp"
#include "../Display/LcdText.hpp"
#include "PC1500Machine.hpp"
#include "PC1500Screenshot.hpp"

// The PC-1500/1500A screen as text -- see Core/Display/LcdText.hpp.
//
// Font: CHARSET at FCA0H, 5 bytes per character for 20H-7FH, column-wise
// (PC-1500_ROM-A0x.lh5801.asm; CHAR_2_ADDR, EE48H, computes FC00H + 5 x
// code). CHAR_OUT_2 (ED5BH) writes the 5 columns plus a blank one, and
// CHAR_OUT advances the column by 6. Codes 80H and up come from the
// second character set at (KATACHAR), which lives outside the ROM, so they
// come out unparsed.
//
// Plain text: ASCII, except where CHARSET draws another sign -- 27H a
// bracket box, 5BH a root, 5CH a yen sign, 5DH a pi, 7FH a full block.
//
// Cursor: WAIT_4_KB (E315H) blinks the cursor cell between glyph 7FH and
// the character saved in BLNKD_CHAR_CODE (787DH); CURSOR_BLNK (787CH) bit 0
// = blinking enabled, bit 7 = 7FH shown now.

inline LcdFont pc1500LcdFont(const PC1500Machine& machine) {
    LcdFont font;
    font.cellHeight = 7;
    const PC1500Memory& mem = machine.memory(); // ROM: never written, safe unlocked
    for (int code = 0x20; code < 0x80; ++code) {
        LcdCell cell{};
        for (int i = 0; i < 5; ++i) cell[i] = mem.peek(uint16_t(0xFCA0 + (code - 0x20) * 5 + i));
        font.glyphs.push_back({uint8_t(code), cell, std::string(1, char(code))});
    }
    const std::pair<uint8_t, char32_t> drawn[] = {
        {0x27, U'\u25A1'}, {0x5B, U'\u221A'}, {0x5C, U'\u00A5'}, {0x5D, U'\u03C0'}, {0x7F, U'\u2588'}};
    for (const auto& [code, ch] : drawn) font.glyphs[code - 0x20].text = utf8(ch);
    font.cursorShapes = {font.glyphs.back().cell}; // 7FH
    return font;
}

/// Thread-safe: RAM through the locked debugPeek(), the screen through the
/// locked display() snapshot.
inline LcdText pc1500LcdText(const PC1500Machine& machine) {
    const LcdFont font = pc1500LcdFont(machine);
    // The cell's position is only kept as a display-RAM address
    // (CURS_POS_NBUF, 787EH); glyph 7FH is no other character's shape, so
    // the first block cell is taken.
    std::optional<LcdCursor> cursor;
    if ((machine.debugPeek(0x787C) & 0x81) == 0x81) {
        const uint8_t code = machine.debugPeek(0x787D);
        cursor = LcdCursor{};
        for (const LcdGlyph& g : font.glyphs)
            if (g.code == code) cursor->under = g.cell;
    }
    LcdText text = parseLcdText(pc1500LcdBitmap(machine), font, cursor);
    text.status = statusWords(machine.display().statusLine());
    return text;
}
