#pragma once
#include <string>

// SHIFT + base key -> punctuation character: the keyboard "second legend"
// row shared by the PC-1500 and PC-1600 (confirmed against real hardware
// by the project owner). On both machines the ROM's key-code table does
// the SHIFT translation, so a caller just needs to drive SHIFT + the
// returned base key.
//
// This is the single C++ source of truth for that table. Callers add
// their own machine-specific extras:
//   * Core/PC1500/PC1500TypedInput.hpp also routes a-z here (SHIFT tap =
//     lowercase on the PC-1500's single-legend letter keys).
//   * Core/PC1600/PC1600TypedInput.hpp also maps the PC-1600 digit-row
//     second legends (' [ ] ` { } \ ~ |) plus SHIFT + . = _, which the
//     PC-1500 lacks.
inline bool sharpShiftedSymbolBaseKey(char c, std::string* base) {
    switch (c) {
        case '!': *base = "f1"; return true;
        case '"': *base = "f2"; return true;
        case '#': *base = "f3"; return true;
        case '$': *base = "f4"; return true;
        case '%': *base = "f5"; return true;
        case '&': *base = "f6"; return true;
        case '<': *base = "(";  return true;
        case '>': *base = ")";  return true;
        case '?': *base = "/";  return true;
        case ':': *base = "*";  return true;
        case ',': *base = "-";  return true;
        case ';': *base = "+";  return true;
        case '@': *base = "=";  return true;
        default: return false;
    }
}

// SHIFT + base key -> the PC-1600 digit-row's own "second legend" (printed
// above the numeric keypad's 0-9), which the PC-1500 keyboard lacks
// entirely (confirmed against real hardware). Header-only like
// sharpShiftedSymbolBaseKey above so it can be shared by both
// Core/PC1600/PC1600BasicTyper.cpp (preset `type:` steps) and the Qt6 GUI's
// interactive keyboard without pulling in PC1600BasicTyper.cpp's optional
// (sharpdx-gated) build unit.
//
// Checked against the ROM's SHIFT-code table (SFTCDT, bank 6 @ 953FH,
// indexed by key code - 08H; see PC-1600-Keyboard.md §7): 1-8 and 0 carry
// these second legends, 9 has none (SHIFT + 9 stays 9), and _ is on the
// "." key -- the one entry here that isn't on the digit row.
inline bool pc1600DigitRowShiftedBaseKey(char c, std::string* base) {
    switch (c) {
        case '^':  *base = "space"; return true;  // caret == SHIFT + SPACE (per real hardware)
        case '\'': *base = "1"; return true;
        case '[':  *base = "2"; return true;
        case ']':  *base = "3"; return true;
        case '`':  *base = "4"; return true;
        case '{':  *base = "5"; return true;
        case '}':  *base = "6"; return true;
        case '\\': *base = "7"; return true;
        case '~':  *base = "8"; return true;
        case '_':  *base = "."; return true;  // SFTCDT: . (2EH) -> _ (5FH)
        case '|':  *base = "0"; return true;  // hardware glyph is a broken vertical bar
        default: return false;
    }
}
