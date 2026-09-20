#include "PC1500KeyboardMap.hpp"

#include "SharpShiftedSymbols.hpp"

namespace PC1500KeyboardMap {

namespace {

std::optional<ResolvedKey> plain(const char* name, bool needsShift = false) {
    return ResolvedKey{name, needsShift};
}

// The 13 shared "second legend" punctuation chars: Core/SharpShiftedSymbols.hpp
// is the single source of truth for this table (also used by the PC-1500/1600
// BASIC typers).
std::optional<ResolvedKey> shiftedCharacterBaseKeyName(QChar c) {
    if (c.unicode() > 0x7f) return std::nullopt;
    std::string base;
    if (!sharpShiftedSymbolBaseKey(static_cast<char>(c.unicode()), &base)) return std::nullopt;
    return ResolvedKey{base, true};
}

// PC-1600-only: the digit-row's own "second legend" (printed above the
// numeric keypad's 0-9), which the PC-1500 keyboard lacks entirely.
// Core/SharpShiftedSymbols.hpp's pc1600DigitRowShiftedBaseKey is the single
// source of truth for this table (also used by Core/PC1600/PC1600BasicTyper.cpp
// for preset `type:` steps); this reuses it rather than re-transcribing it.
std::optional<ResolvedKey> pc1600DigitRowShiftedBaseKeyName(QChar c) {
    if (c.unicode() > 0x7f) return std::nullopt;
    std::string base;
    if (!pc1600DigitRowShiftedBaseKey(static_cast<char>(c.unicode()), &base)) return std::nullopt;
    return ResolvedKey{base, true};
}

std::optional<ResolvedKey> characterName(QChar c) {
    if (c == ' ') return plain("space");
    if (c.isLetter()) {
        static char buf[2] = {0, 0};
        buf[0] = c.toLower().toLatin1();
        return plain(buf);
    }
    if (c.isDigit()) {
        static char buf[2] = {0, 0};
        buf[0] = c.toLatin1();
        return plain(buf);
    }
    switch (c.unicode()) {
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

std::optional<ResolvedKey> resolve(Qt::Key key, Qt::KeyboardModifiers modifiers,
                                    const QString& text, bool isPC1600) {
#ifdef Q_OS_MACOS
    // Qt maps Cmd to ControlModifier and the Control key to MetaModifier.
    if (modifiers & (Qt::ControlModifier | Qt::MetaModifier)) return std::nullopt;
#else
    if (modifiers & Qt::MetaModifier) return std::nullopt;
    const bool ctrl = modifiers & Qt::ControlModifier;
    const bool alt = modifiers & Qt::AltModifier;
    if (ctrl != alt) return std::nullopt;  // Ctrl or Alt alone; both together = AltGr
#endif
    if (key == Qt::Key_Backspace) {
        return plain(isPC1600 ? "bs" : "left");
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F6) {
        static const char* names[] = {"f1", "f2", "f3", "f4", "f5", "f6"};
        return plain(names[key - Qt::Key_F1]);
    }

    // Extended/navigation-cluster keys, only reachable via an external/
    // PC-style keyboard -- the project's own chosen PC-1500 equivalents,
    // not hardware-documented correspondences.
    switch (key) {
    case Qt::Key_ScrollLock: return plain("rsv");
    case Qt::Key_Home: return plain("rcl");
    case Qt::Key_End: return plain("sml");
    case Qt::Key_PageUp: return plain("left", true);
    case Qt::Key_PageDown: return plain("right", true);
    default: break;
    }

    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter: return plain("enter");
    case Qt::Key_Up: return plain("up");
    case Qt::Key_Down: return plain("down");
    case Qt::Key_Left: return plain("left");
    case Qt::Key_Right: return plain("right");
    case Qt::Key_Delete: return plain("cl");
    case Qt::Key_Tab: return plain("mode");
    default: break;
    }

    if (text.size() == 1) {
        if (auto r = characterName(text.at(0))) return r;
        if (auto r = shiftedCharacterBaseKeyName(text.at(0))) return r;
        if (isPC1600) {
            if (auto r = pc1600DigitRowShiftedBaseKeyName(text.at(0))) return r;
        }
    }

    return std::nullopt;
}

} // namespace PC1500KeyboardMap
