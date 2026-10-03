#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "LcdScreenshot.hpp"

// ── LCD text: dot matrix -> characters, by exact glyph lookup ────────────
//
// Both machines print text on a fixed grid of 6-dot-wide cells (26 per
// 156-dot row): the PC-1500 in one 7-dot row, the PC-1600 in four 8-dot
// rows. The font comes from the machine's own ROM (see
// PC1500/PC1500LcdText.hpp, PC1600/PC1600LcdText.hpp), so a cell is either
// exactly one of its glyphs or not text at all -- there is no guessing.
//
// Per cell, in order: all dots off -> space; while the model reports a
// cursor, the cursor cell if it shows one of the font's cursor shapes --
// the ROMs draw the cursor *over* the cell, so the character underneath is
// decoded from `LcdCursor::under`, the ROM's own save cell; a glyph; an
// inverted glyph (reverse video, counted in `reverseCells`).
// Anything else is graphics, a redefined character or text placed
// off the grid: it is written as U+FFFD and counted in `unparsed`, which
// tells a caller to fall back to the PNG.
//
// Encoding: printable ASCII as is, except `\` -> `\\`; any other code the
// font has (katakana, graphic characters, 7FH) as `\xHH`.
//
// Model-free, like LcdScreenshot: the per-model font and status extraction
// lives in the model directories.

/// One cell's dots, column-wise: bit n of a byte = row n of the cell.
using LcdCell = std::array<uint8_t, 8>;

struct LcdFont {
    int cellWidth = 6;
    int cellHeight = 8;          ///< 7 (PC-1500) or 8 (PC-1600)
    /// Character code -> its cell. On a duplicate cell the first entry
    /// wins, so builders add printable ASCII first.
    std::vector<std::pair<uint8_t, LcdCell>> glyphs;
    std::vector<LcdCell> cursorShapes;
};

struct LcdText {
    std::vector<std::string> rows;
    std::vector<std::string> status; ///< lit status symbols, left to right (filled by the model adapter)
    int unparsed = 0;                ///< cells that are not text
    int reverseCells = 0;
    int cursorRow = -1;              ///< -1: no cursor on screen
    int cursorCol = -1;
    bool poweredOn = true;

    /// The rows joined with '\n' (no trailing newline).
    std::string text() const;
    /// True if `needle` occurs in any one row.
    bool contains(const std::string& needle) const;
    /// rows, then "status: ..." and, when nonzero, "unparsed: n" -- the
    /// --lcd-text / log format.
    std::string report() const;
};

/// The angle-mode legend as the panel reads: its DE, G and RAD segments
/// light in pairs (DE+G = "DEG", G+RAD = "GRAD") or alone ("RAD"), so they
/// are joined left to right. Empty when none is lit.
inline std::string lcdAngleLegend(bool de, bool g, bool rad) {
    return std::string(de ? "DE" : "") + (g ? "G" : "") + (rad ? "RAD" : "");
}

/// A cursor the model reports as shown.
struct LcdCursor {
    /// Its cell. -1/-1: position unknown, the first cursor-shaped cell is
    /// taken -- only safe when no glyph has a cursor shape (the PC-1600's
    /// underline cursor is also its `_`).
    int row = -1;
    int col = -1;
    LcdCell under{}; ///< the cell the ROM saved before drawing the cursor
};

LcdText parseLcdText(const LcdBitmap& bitmap, const LcdFont& font,
                     const std::optional<LcdCursor>& cursor = std::nullopt);

/// report() to `path`, or to stdout for "-". False with `error` set if the
/// file can't be written.
bool writeLcdTextReport(const LcdText& text, const std::string& path, std::string* error);
