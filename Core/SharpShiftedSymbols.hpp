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
//   * Core/PC1500/PC1500BasicTyper.cpp also routes a-z here (SHIFT tap =
//     lowercase on the PC-1500's single-legend letter keys).
//   * Core/PC1600/PC1600PresetLoader.cpp also maps the PC-1600 digit-row
//     second legends (' [ ] ` { } \ ~ _ |), which the PC-1500 lacks.
//
// The Swift GUI keeps its own copy (Calc-U-1600/PC1500KeyboardMap.swift's
// shiftedCharacterBaseKeyName) -- a different layer/language; keep the two
// in sync by hand until the input paths are unified.
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
        case '_':  *base = "9"; return true;
        case '|':  *base = "0"; return true;  // hardware glyph is a broken vertical bar
        default: return false;
    }
}
