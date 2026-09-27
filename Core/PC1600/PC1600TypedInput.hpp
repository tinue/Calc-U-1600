#pragma once
#include <cctype>
#include <cstdint>
#include <string>

#include "../SharpShiftedSymbols.hpp"
#include "PC1600Keyboard.hpp"
#include "PC1600Machine.hpp"

// ── Typed-text helpers shared by every scripted PC-1600 input path ───────
//
// The PC-1600 analog of Core/PC1500/PC1500TypedInput.hpp: used by
// PC1600BasicTyper (preset `type:` steps, BASIC program text) and the
// GUI's clipboard paste (Core/KeyPaste).

/// Maps a host character to the PC-1600 key that types it. `needsShift`
/// is set when SHIFT must be tapped first (a one-shot latch), in
/// precedence order:
///   1. a-z            -> SHIFT + the uppercase letter key (lowercase)
///   2. the 13 shared "second legend" punctuation chars (SharpShiftedSymbols)
///   3. the PC-1600 digit-row second legends (incl. '^' = SHIFT + SPACE),
///      which the PC-1500 keyboard lacks.
/// This is NOT the SML lock (a separate persistent lowercase/kana toggle).
/// Returns false for a character with no key.
inline bool pc1600ResolveTypedChar(char c, std::string* baseKey, bool* needsShift) {
    if (c >= 'a' && c <= 'z') {
        *baseKey = std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        *needsShift = true;
        return true;
    }
    if (sharpShiftedSymbolBaseKey(c, baseKey) || pc1600DigitRowShiftedBaseKey(c, baseKey)) {
        *needsShift = true;
        return true;
    }
    *needsShift = false;
    if (c == ' ') {
        *baseKey = "space";
    } else {
        *baseKey = std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return PC1600Keyboard::keyFromName(*baseKey) != PC1600Keyboard::Key::Unknown;
}

// ── KBII: the accented / international characters ───────────────────────
//
// KBII is a latching key (like SML): while it is on, KEYCONV (P1-B3 48A7H)
// passes a letter or ( ) through the KBII table KYCDKB2 (P2-B6 9592H), or,
// with SHIFT, through KYCDSK2 (95E5H). KBII is tested before SML, so SML
// doesn't change the result. The codes are the CP437 layout (libsharpdx
// and the CE-1600P font use the same). A host character is typed as
// KBII, [SHIFT,] key, KBII -- one self-contained sequence per character,
// so KBII is never left latched.
//
// SHIFT+KBII is not "lowercase KBII" throughout: for 8 keys it gives the
// lowercase partner (Ä/ä, ...), for 8 the same character, and for 12 a
// different one (B ù/û, H ¡/½, ...). One row per distinct character; a
// character in both columns is listed once, unshifted.
struct Pc1600KbiiChar {
    char32_t ch;
    char key;   // the letter / paren key, PC1600Keyboard vocabulary
    bool shift; // from KYCDSK2
};

inline constexpr Pc1600KbiiChar kPc1600KbiiChars[] = {
    // KYCDKB2 (KBII)
    {U'á', 'A', false}, {U'ù', 'B', false}, {U'ì', 'C', false}, {U'í', 'D', false},
    {U'ï', 'E', false}, {U'ó', 'F', false}, {U'ú', 'G', false}, {U'¡', 'H', false},
    {U'Å', 'I', false}, {U'¿', 'J', false}, {U'ª', 'K', false}, {U'º', 'L', false},
    {U'¢', 'M', false}, {U'£', 'N', false}, {U'Ñ', 'O', false}, {U'Ç', 'P', false},
    {U'Ä', 'Q', false}, {U'Ö', 'R', false}, {U'É', 'S', false}, {U'Ü', 'T', false},
    {U'Æ', 'U', false}, {U'ò', 'V', false}, {U'ë', 'W', false}, {U'è', 'X', false},
    {U'ÿ', 'Y', false}, {U'à', 'Z', false}, {U'₧', '(', false}, {U'ƒ', ')', false},
    // KYCDSK2 (SHIFT+KBII), where it differs from KYCDKB2
    {U'û', 'B', true}, {U'î', 'C', true}, {U'½', 'H', true}, {U'å', 'I', true},
    {U'¼', 'J', true}, {U'⌐', 'K', true}, {U'¬', 'L', true}, {U'¥', 'N', true},
    {U'ñ', 'O', true}, {U'ç', 'P', true}, {U'ä', 'Q', true}, {U'ö', 'R', true},
    {U'é', 'S', true}, {U'ü', 'T', true}, {U'æ', 'U', true}, {U'ô', 'V', true},
    {U'ê', 'X', true}, {U'â', 'Z', true}, {U'«', '(', true}, {U'»', ')', true},
};

inline const Pc1600KbiiChar* pc1600FindKbiiChar(char32_t cp) {
    for (const Pc1600KbiiChar& c : kPc1600KbiiChars)
        if (c.ch == cp) return &c;
    return nullptr;
}

/// The other-case form of a Latin-1 letter (É <-> é, ÿ <-> Ÿ), or 0.
inline char32_t latin1CasePartner(char32_t cp) {
    if (cp == U'ÿ') return U'Ÿ';
    if (cp == U'Ÿ') return U'ÿ';
    if (cp == U'×' || cp == U'÷' || cp == U'ß') return 0;
    if (cp >= 0xC0 && cp <= 0xDE) return cp + 0x20;
    if (cp >= 0xE0 && cp <= 0xFE) return cp - 0x20;
    return 0;
}

/// Maps a host character to its KBII key. Case-exact (`foldCase` false,
/// paste and `type:`): the character itself, or -- for an uppercase form
/// the ROM lacks (Ë, Û) -- its case partner. Case-folded (`foldCase` true,
/// live host keys, which type the way the calculator's own keys do): where
/// both cases exist (Ä/ä, É/é, ...), always the unshifted uppercase one.
/// Returns false for a character with no KBII key.
inline bool pc1600ResolveKbiiChar(char32_t cp, bool foldCase, std::string* baseKey, bool* needsShift) {
    const Pc1600KbiiChar* self = pc1600FindKbiiChar(cp);
    const Pc1600KbiiChar* partner = pc1600FindKbiiChar(latin1CasePartner(cp));
    const Pc1600KbiiChar* hit = self ? self : partner;
    if (foldCase && self && partner && self->shift) hit = partner;
    if (!hit) return false;
    *baseKey = std::string(1, hit->key);
    *needsShift = hit->shift;
    return true;
}

/// buildPasteSteps' non-ASCII resolver for the PC-1600 (case-exact).
inline bool pc1600ResolveTypedKbiiChar(char32_t cp, std::string* baseKey, bool* needsShift) {
    return pc1600ResolveKbiiChar(cp, /*foldCase=*/false, baseKey, needsShift);
}

/// True when the SC7852 is in the BASIC command loop right now -- a
/// single-frame sample; callers require it to hold for a stretch. The loop
/// is pinned to ~$92B3 (measured: ~97% of frames at the prompt); a running
/// program, an INPUT wait, a plot pen-move wait and a FOR/NEXT delay all
/// sit in other tight loops ($53xx, $AAxx, ...). The PC-1600 ROM set is
/// fixed, so the window is stable.
inline bool pc1600AtBasicPrompt(const PC1600Machine& machine) {
    constexpr uint16_t kCmdLoopLo = 0x9280;
    constexpr uint16_t kCmdLoopHi = 0x9300;
    const uint16_t pc = machine.sc7852().pc();
    return machine.sc7852Owns() && pc >= kCmdLoopLo && pc <= kCmdLoopHi;
}
