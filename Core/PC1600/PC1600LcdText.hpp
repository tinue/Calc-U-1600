#pragma once
#include <cstdint>
#include <optional>

#include "../Display/LcdCharsets.hpp"
#include "../Display/LcdText.hpp"
#include "PC1600Machine.hpp"
#include "PC1600Screenshot.hpp"

// The PC-1600 screen as text -- see Core/Display/LcdText.hpp.
//
// Font: bank 6's 6x8 ROM font, 6 bytes per character, column-wise
// (notes/PC1600-P2-B6-Disassembly.md). Its address differs between the
// ROM versions (CGLOW: new ADA4H, old ADE3H), so it is read from the ROM:
// the jump-table entry at 8030H leads to CGSET80, whose `ld de,CGHIGH`
// (+4) restores the 80H-FFH font; CGLOW (20H-7FH) is the 60H x 6 bytes
// before it and CGSPEC (the PC-1500-style 1EH/27H/5BH/5DH) the 4 x 6
// before that. User CGs (CGSET80 with DE <> 0, UPACGA) are not followed:
// redefined characters come out unparsed.
//
// Plain text: CGLOW is ASCII with a full block at 7FH; CGHIGH draws the
// CP437 upper half in both revisions. CGSPEC (AD8CH new / ADCBH old)
// draws a bracket box, a root, a pi and a fourth sign with no Unicode
// counterpart (a blank in plain text).
//
// Cursor: CRSRDRAW (bank 6, 8543H) overwrites the cell with five columns
// of 40H (underline, CRSRST 1) or 7FH (block, CRSRST 2) and a blank one,
// after saving the cell's columns in CRSRSAV (F069H), at screen row CRSRY
// (F05FH) / column CRSRX (F060H). It is shown while LCDWK2 (F05EH) bit 0
// is set. The underline is also the `_` glyph, so only that cell is
// checked.

inline LcdFont pc1600LcdFont(const PC1600Machine& machine) {
    LcdFont font;
    font.cellHeight = 8;
    const uint8_t* rom = machine.memory().bank6Rom();
    if (!rom) return font;
    auto at = [rom](uint16_t addr) { return rom[(addr - 0x8000) & (PC1600Memory::kBankSize - 1)]; };
    auto word = [&](uint16_t addr) { return uint16_t(at(addr) | at(uint16_t(addr + 1)) << 8); };
    if (at(0x8030) != 0xC3) return font;          // jp CGSET80
    const uint16_t cgset80 = word(0x8031);
    if (at(uint16_t(cgset80 + 4)) != 0x11) return font; // ld de,CGHIGH
    const uint16_t cgHigh = word(uint16_t(cgset80 + 5));
    const uint16_t cgLow = uint16_t(cgHigh - 0x60 * 6);
    const uint16_t cgSpec = uint16_t(cgLow - 4 * 6);
    auto cell = [&](uint16_t addr) {
        LcdCell c{};
        for (int i = 0; i < 6; ++i) c[i] = at(uint16_t(addr + i));
        return c;
    };
    for (int code = 0x20; code < 0x80; ++code)
        font.glyphs.push_back({uint8_t(code), cell(uint16_t(cgLow + (code - 0x20) * 6)),
                               code == 0x7F ? utf8(U'\u2588') : std::string(1, char(code))});
    for (int code = 0x80; code < 0x100; ++code)
        font.glyphs.push_back({uint8_t(code), cell(uint16_t(cgHigh + (code - 0x80) * 6)), utf8(kCp437High[code - 0x80])});
    const uint8_t specCodes[] = {0x1E, 0x27, 0x5B, 0x5D};
    const char32_t specText[] = {U'\u25A1', U'\u221A', U'\u03C0', 0}; // □ √ π, -
    for (int i = 0; i < 4; ++i)
        font.glyphs.push_back({specCodes[i], cell(uint16_t(cgSpec + i * 6)), specText[i] ? utf8(specText[i]) : std::string()});
    font.cursorShapes = {LcdCell{0x40, 0x40, 0x40, 0x40, 0x40, 0x00}, LcdCell{0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x00}};
    return font;
}

/// Thread-safe: RAM through the locked debugPeek(), the screen through the
/// locked displaySnapshot(), the font from ROM.
inline LcdText pc1600LcdText(PC1600Machine& machine) {
    std::optional<LcdCursor> cursor;
    if (machine.debugPeek(0xF05E) & 0x01) {
        cursor = LcdCursor{};
        cursor->row = machine.debugPeek(0xF05F);
        cursor->col = machine.debugPeek(0xF060);
        for (int i = 0; i < 6; ++i) cursor->under[i] = machine.debugPeek(uint16_t(0xF069 + i));
    }
    LcdText text = parseLcdText(pc1600LcdBitmap(machine), pc1600LcdFont(machine), cursor);
    const PC1600DisplaySnapshot snap = machine.displaySnapshot();
    StatusLine line;
    for (std::size_t i = 0; i < StatusLine::kCount; ++i) line.set(static_cast<StatusLine::Symbol>(i), snap.statusSymbols[i]);
    text.status = statusWords(line);
    return text;
}
