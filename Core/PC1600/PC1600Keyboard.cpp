#include "PC1600Keyboard.hpp"

namespace {

// Matrix position (strobe 0-8, sense bit 0-7) for each key, transcribed
// directly from PC-1600-Keyboard.md §5's table (strobe 8 == the PB6 line).
struct Pos { int strobe; int bit; };

Pos positionOf(PC1600Keyboard::Key k) {
    using K = PC1600Keyboard::Key;
    switch (k) {
        case K::Digit2: return {0, 0};
        case K::Digit5: return {0, 1};
        case K::Digit8: return {0, 2};
        case K::H:      return {0, 3};
        case K::Shift:  return {0, 4};
        case K::Y:      return {0, 5};
        case K::N:      return {0, 6};
        case K::Up:     return {0, 7};

        case K::Period: return {1, 0};
        case K::Minus:  return {1, 1};
        case K::Off:    return {1, 2};
        case K::S:      return {1, 3};
        case K::F1:     return {1, 4};
        case K::W:      return {1, 5};
        case K::X:      return {1, 6};
        case K::Rsv:    return {1, 7};

        case K::Digit1: return {2, 0};
        case K::Digit4: return {2, 1};
        case K::Digit7: return {2, 2};
        case K::J:      return {2, 3};
        case K::F5:     return {2, 4};
        case K::U:      return {2, 5};
        case K::M:      return {2, 6};
        case K::Digit0: return {2, 7};

        case K::RightParen: return {3, 0};
        case K::L:           return {3, 1};
        case K::O:            return {3, 2};
        case K::K:            return {3, 3};
        case K::F6:           return {3, 4};
        case K::I:             return {3, 5};
        case K::LeftParen:    return {3, 6};
        case K::Enter:        return {3, 7};

        case K::Plus:     return {4, 0};
        case K::Asterisk: return {4, 1};
        case K::Slash:    return {4, 2};
        case K::D:        return {4, 3};
        case K::F2:       return {4, 4};
        case K::E:        return {4, 5};
        case K::C:        return {4, 6};
        case K::Rcl:      return {4, 7};

        case K::Equals: return {5, 0};
        case K::Left:   return {5, 1};
        case K::P:      return {5, 2};
        case K::F:      return {5, 3};
        case K::F3:     return {5, 4};
        case K::R:      return {5, 5};
        case K::V:      return {5, 6};
        case K::Space:  return {5, 7};

        case K::Right: return {6, 0};
        case K::Mode:  return {6, 1};
        case K::Cl:    return {6, 2};
        case K::A:     return {6, 3};
        case K::Def:   return {6, 4};
        case K::Q:     return {6, 5};
        case K::Z:     return {6, 6};
        case K::Sml:   return {6, 7};

        case K::Digit3: return {7, 0};
        case K::Digit6: return {7, 1};
        case K::Digit9: return {7, 2};
        case K::G:      return {7, 3};
        case K::F4:     return {7, 4};
        case K::T:      return {7, 5};
        case K::B:      return {7, 6};
        case K::Down:   return {7, 7};

        case K::Ctrl: return {8, 0};
        case K::Kbii: return {8, 1};
        case K::Bs:   return {8, 2};

        default: return {-1, -1}; // Unknown
    }
}

} // namespace

PC1600Keyboard::Key PC1600Keyboard::keyFromName(const std::string& name) {
    using K = Key;
    if (name.size() == 1) {
        char c = name[0];
        if (c >= '0' && c <= '9') return static_cast<K>(static_cast<int>(K::Digit0) + (c - '0'));
        if (c >= 'a' && c <= 'z') return static_cast<K>(static_cast<int>(K::A) + (c - 'a'));
        if (c >= 'A' && c <= 'Z') return static_cast<K>(static_cast<int>(K::A) + (c - 'A'));
        switch (c) {
            case '+': return K::Plus;
            case '-': return K::Minus;
            case '=': return K::Equals;
            case '*': return K::Asterisk;
            case '/': return K::Slash;
            case '(': return K::LeftParen;
            case ')': return K::RightParen;
            case '.': return K::Period;
            default: return K::Unknown;
        }
    }
    if (name == "up") return K::Up;
    if (name == "down") return K::Down;
    if (name == "left") return K::Left;
    if (name == "right") return K::Right;
    if (name == "shift") return K::Shift;
    if (name == "sml") return K::Sml;
    if (name == "mode") return K::Mode;
    if (name == "def") return K::Def;
    if (name == "off") return K::Off;
    if (name == "cl") return K::Cl;
    if (name == "rcl") return K::Rcl;
    if (name == "enter" || name == "ent") return K::Enter;
    if (name == "space") return K::Space;
    if (name == "rsv") return K::Rsv;
    if (name == "f1") return K::F1;
    if (name == "f2") return K::F2;
    if (name == "f3") return K::F3;
    if (name == "f4") return K::F4;
    if (name == "f5") return K::F5;
    if (name == "f6") return K::F6;
    if (name == "ctrl") return K::Ctrl;
    if (name == "kbii") return K::Kbii;
    if (name == "bs") return K::Bs;
    return K::Unknown;
}

void PC1600Keyboard::setKeyState(Key key, bool pressed) {
    Pos p = positionOf(key);
    if (p.strobe < 0) return; // Unknown / no matrix position (e.g. ON)
    m_keys[static_cast<size_t>(p.strobe)][static_cast<size_t>(p.bit)] = pressed;
}

uint8_t PC1600Keyboard::scan(uint8_t opaDriveLines, bool pb6Active) const {
    ++m_scanCount;
    uint8_t sense = 0xFF;
    for (int s = 0; s < 8; s++) {
        if (opaDriveLines & (1 << s)) continue; // active-low: bit set means NOT strobed
        for (int b = 0; b < kSenseBits; b++) {
            if (m_keys[static_cast<size_t>(s)][static_cast<size_t>(b)]) sense &= uint8_t(~(1 << b));
        }
    }
    if (pb6Active) {
        for (int b = 0; b < kSenseBits; b++) {
            if (m_keys[8][static_cast<size_t>(b)]) sense &= uint8_t(~(1 << b));
        }
    }
    return sense;
}
