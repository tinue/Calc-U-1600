#include "Shared/HostKeyMap.hpp"

#include "PC1600/PC1600TypedInput.hpp"
#include "SharpShiftedSymbols.hpp"

namespace shared {

namespace {

std::optional<ResolvedKey> plain(std::string name, bool needsShift = false) {
    return ResolvedKey{std::move(name), needsShift};
}

// The 13 shared "second legend" punctuation chars: Core/SharpShiftedSymbols.hpp
// is the single source of truth for this table (also used by the PC-1500/1600
// BASIC typers).
std::optional<ResolvedKey> shiftedCharacterBaseKeyName(char c) {
    std::string base;
    if (!sharpShiftedSymbolBaseKey(c, &base)) return std::nullopt;
    return ResolvedKey{base, true};
}

// PC-1600-only: the digit-row's own "second legend" (printed above the
// numeric keypad's 0-9), which the PC-1500 keyboard lacks entirely.
// Core/SharpShiftedSymbols.hpp's pc1600DigitRowShiftedBaseKey is the single
// source of truth for this table (also used by Core/PC1600/PC1600BasicTyper.cpp
// for preset `type:` steps); this reuses it rather than re-transcribing it.
std::optional<ResolvedKey> pc1600DigitRowShiftedBaseKeyName(char c) {
    std::string base;
    if (!pc1600DigitRowShiftedBaseKey(c, &base)) return std::nullopt;
    return ResolvedKey{base, true};
}

std::optional<ResolvedKey> characterName(char c) {
    if (c == ' ') return plain("space");
    if (c >= 'a' && c <= 'z') return plain(std::string(1, c));
    if (c >= 'A' && c <= 'Z') return plain(std::string(1, static_cast<char>(c - 'A' + 'a')));
    if (c >= '0' && c <= '9') return plain(std::string(1, c));
    switch (c) {
    case '+': return plain("+");
    case '-': return plain("-");
    case '=': return plain("=");
    case '*': return plain("*");
    case '/': return plain("/");
    case '(': return plain("(");
    case ')': return plain(")");
    case '.': return plain(".");
    default: return std::nullopt;
    }
}

} // namespace

std::optional<ResolvedKey> resolveHostKey(HostKey key, HostModifiers modifiers, char32_t typed, bool isPC1600,
                                          bool macHost) {
    if (macHost) {
        if (modifiers.command || modifiers.control) return std::nullopt;
    } else {
        if (modifiers.meta) return std::nullopt;
        if (modifiers.control != modifiers.alt) return std::nullopt;  // Control or Alt alone; both = AltGr
    }

    switch (key) {
    case HostKey::Backspace: return plain(isPC1600 ? "bs" : "left");
    case HostKey::F1: return plain("f1");
    case HostKey::F2: return plain("f2");
    case HostKey::F3: return plain("f3");
    case HostKey::F4: return plain("f4");
    case HostKey::F5: return plain("f5");
    case HostKey::F6: return plain("f6");

    // Extended/navigation-cluster keys, only reachable via an external/
    // PC-style keyboard -- the project's own chosen PC-1500 equivalents,
    // not hardware-documented correspondences. (Insert sits where a
    // Mac-layout keyboard's Scroll Lock is.)
    case HostKey::ScrollLock: return plain("rsv");
    case HostKey::Insert: return plain("rsv");
    case HostKey::Home:
        // CTRL exists only on the PC-1600; elsewhere Shift+Home = Home.
        if (isPC1600 && modifiers.shift) return plain("ctrl");
        return plain("rcl");
    case HostKey::End:
        // KB II exists only on the PC-1600; elsewhere Shift+End = End.
        if (isPC1600 && modifiers.shift) return plain("kbii");
        return plain("sml");
    case HostKey::PageUp:
        if (modifiers.shift) return plain("def");
        return plain("left", true);
    case HostKey::PageDown: return plain("right", true);

    case HostKey::Return:
    case HostKey::Enter: return plain("enter");
    case HostKey::Up: return plain("up");
    case HostKey::Down: return plain("down");
    case HostKey::Left: return plain("left");
    case HostKey::Right: return plain("right");
    case HostKey::Delete: return plain("cl", modifiers.shift);
    case HostKey::Tab: return plain("mode");
    case HostKey::Other: break;
    }

    if (typed == 0) return std::nullopt;
    // PC-1600 accented characters, typed as typed (é gives é, É gives É)
    // -- unlike the letters, whose uppercase default is for BASIC.
    if (isPC1600 && typed > 0x7f) {
        ResolvedKey r;
        r.needsKbii = pc1600ResolveKbiiChar(typed, &r.baseKey, &r.needsShift);
        if (r.needsKbii) return r;
    }
    if (typed > 0x7f) return std::nullopt;
    const char c = static_cast<char>(typed);
    if (auto r = characterName(c)) return r;
    if (auto r = shiftedCharacterBaseKeyName(c)) return r;
    if (isPC1600) {
        if (auto r = pc1600DigitRowShiftedBaseKeyName(c)) return r;
    }
    return std::nullopt;
}

} // namespace shared
