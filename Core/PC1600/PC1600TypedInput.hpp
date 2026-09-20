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
