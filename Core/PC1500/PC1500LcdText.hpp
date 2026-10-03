#pragma once
#include <cstdint>
#include <optional>

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
// Cursor: WAIT_4_KB (E315H) blinks the cursor cell between glyph 7FH and
// the character saved in BLNKD_CHAR_CODE (787DH); CURSOR_BLNK (787CH) bit 0
// = blinking enabled, bit 7 = 7FH shown now.

inline LcdFont pc1500LcdFont(const PC1500Machine& machine) {
    LcdFont font;
    font.cellHeight = 7;
    const PC1500Memory& mem = machine.memory();
    for (int code = 0x20; code < 0x80; ++code) {
        LcdCell cell{};
        for (int i = 0; i < 5; ++i) cell[i] = mem.peek(uint16_t(0xFCA0 + (code - 0x20) * 5 + i));
        font.glyphs.emplace_back(uint8_t(code), cell);
    }
    font.cursorShapes = {font.glyphs.back().second}; // 7FH
    return font;
}

/// Unlocked (reads RAM through memory()): headless tools and tests, or a
/// caller already holding the machine.
inline LcdText pc1500LcdText(const PC1500Machine& machine) {
    const PC1500Memory& mem = machine.memory();
    const LcdFont font = pc1500LcdFont(machine);
    // The cell's position is only kept as a display-RAM address
    // (CURS_POS_NBUF, 787EH); glyph 7FH is no other character's shape, so
    // the first block cell is taken.
    std::optional<LcdCursor> cursor;
    if ((mem.peek(0x787C) & 0x81) == 0x81) {
        const uint8_t code = mem.peek(0x787D);
        cursor = LcdCursor{};
        for (const auto& [c, cell] : font.glyphs)
            if (c == code) cursor->under = cell;
    }
    LcdText text = parseLcdText(pc1500LcdBitmap(machine), font, cursor);
    const PC1500Display disp = machine.display();
    for (const auto& [name, on] : disp.statusSymbols()) {
        const std::string n = name;
        if (n == "DE" || n == "G" || n == "RAD") {
            if (n == "DE" && !lcdAngleLegend(disp.de(), disp.g(), disp.rad()).empty())
                text.status.push_back(lcdAngleLegend(disp.de(), disp.g(), disp.rad()));
            continue;
        }
        if (on) text.status.push_back(n);
    }
    return text;
}
