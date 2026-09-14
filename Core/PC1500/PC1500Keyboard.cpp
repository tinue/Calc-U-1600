#include "PC1500Keyboard.hpp"

namespace {
using Key = PC1500Keyboard::Key;

// Physical matrix [row=IN0-7][col=PA0-7], confirmed by live hardware
// testing on a real PC-1500 (see PC1500Keyboard.hpp's doc comment). IN7/
// PA1 holds the distinct RSV (RESERVE) rocker-switch key (see Key::Rsv's
// own doc comment), physically located between SML and RCL on the real
// keyboard.
constexpr Key kMatrix[8][8] = {
    /* IN0 */ {Key::Digit2, Key::Period,     Key::Digit1, Key::RightParen, Key::Plus,     Key::Equals, Key::Right, Key::Digit3},
    /* IN1 */ {Key::Digit5, Key::Minus,      Key::Digit4, Key::L,          Key::Asterisk, Key::Left,   Key::Mode,  Key::Digit6},
    /* IN2 */ {Key::Digit8, Key::Off,        Key::Digit7, Key::O,          Key::Slash,    Key::P,      Key::Cl,    Key::Digit9},
    /* IN3 */ {Key::H,      Key::S,          Key::J,      Key::K,          Key::D,        Key::F,      Key::A,     Key::G},
    /* IN4 */ {Key::Shift,  Key::F1,         Key::F5,     Key::F6,         Key::F2,       Key::F3,     Key::Def,   Key::F4},
    /* IN5 */ {Key::Y,      Key::W,          Key::U,      Key::I,          Key::E,        Key::R,      Key::Q,     Key::T},
    /* IN6 */ {Key::N,      Key::X,          Key::M,      Key::LeftParen,  Key::C,        Key::V,      Key::Z,     Key::B},
    /* IN7 */ {Key::Up,     Key::Rsv,        Key::Digit0, Key::Ent,        Key::Rcl,      Key::Space,  Key::Sml,   Key::Down},
};

bool findPosition(Key key, int& outRow, int& outCol) {
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if (kMatrix[row][col] == key) { outRow = row; outCol = col; return true; }
        }
    }
    return false;
}
} // namespace

PC1500Keyboard::Key PC1500Keyboard::keyFromName(const std::string& name) {
    if (name.size() == 1) {
        char c = name[0];
        if (c >= '0' && c <= '9') return static_cast<Key>(static_cast<int>(Key::Digit0) + (c - '0'));
        if (c >= 'a' && c <= 'z') return static_cast<Key>(static_cast<int>(Key::A) + (c - 'a'));
        if (c >= 'A' && c <= 'Z') return static_cast<Key>(static_cast<int>(Key::A) + (c - 'A'));
        switch (c) {
            case '+': return Key::Plus;
            case '-': return Key::Minus;
            case '=': return Key::Equals;
            case '*': return Key::Asterisk;
            case '/': return Key::Slash;
            case '(': return Key::LeftParen;
            case ')': return Key::RightParen;
            case '.': return Key::Period;
            default: return Key::Unknown;
        }
    }
    if (name == "cl") return Key::Cl;
    if (name == "enter" || name == "ent") return Key::Ent;
    if (name == "mode") return Key::Mode;
    if (name == "def") return Key::Def;
    if (name == "sml") return Key::Sml;
    if (name == "rcl") return Key::Rcl;
    if (name == "shift") return Key::Shift;
    if (name == "off") return Key::Off;
    if (name == "up") return Key::Up;
    if (name == "down") return Key::Down;
    if (name == "rsv") return Key::Rsv;
    if (name == "left") return Key::Left;
    if (name == "right") return Key::Right;
    if (name == "space") return Key::Space;
    if (name == "f1") return Key::F1;
    if (name == "f2") return Key::F2;
    if (name == "f3") return Key::F3;
    if (name == "f4") return Key::F4;
    if (name == "f5") return Key::F5;
    if (name == "f6") return Key::F6;
    return Key::Unknown;
}

void PC1500Keyboard::setKeyState(Key key, bool pressed) {
    int row, col;
    if (findPosition(key, row, col)) m_keys[row][col] = pressed;
}

void PC1500Keyboard::setKeyState(int row, int col, bool pressed) {
    if (row >= 0 && row < kRows && col >= 0 && col < kCols) m_keys[row][col] = pressed;
}

uint8_t PC1500Keyboard::scan(uint8_t driveLines) const {
    uint8_t result = 0xFF;
    for (int col = 0; col < kCols; col++) {
        if (driveLines & (1 << col)) continue; // this column isn't strobed (active-low)
        for (int row = 0; row < kRows; row++) {
            if (m_keys[row][col]) result &= static_cast<uint8_t>(~(1 << row));
        }
    }
    return result;
}
