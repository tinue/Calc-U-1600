#pragma once
#include <array>
#include <cstdint>
#include <string>

// ── PC-1600 physical keyboard matrix ─────────────────────────────────────
//
// 9 strobes (KS0-KS7 = OPA port 1EH's 8 bits, plus one extra strobe on OPB
// port 1FH bit 6 for CTRL/KBII/BS only) x 8 sense bits (a read of I/O port
// 37H). Both directions active-low, per
// SharpPC1500Reference/PC-1600/PC-1600-Keyboard.md §2/§5 (this class only
// needs matrix positions, not what character code the ROM assigns each
// key via its own key-code translation table, §10.2).
//
// ON/BREAK is deliberately NOT part of this matrix (comes from the sub-CPU
// on OPB bit 7 instead, its own interrupt latch) -- modeled the same way
// PC1500Keyboard leaves the PC-1500's ON key out of its matrix.
class PC1600Keyboard {
public:
    enum class Key {
        Digit0, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,
        A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
        Plus, Minus, Equals, Asterisk, Slash, LeftParen, RightParen, Period,
        Left, Right, Up, Down,
        Shift, Sml, Mode, Def, Off,
        /// CL -- named "cl" here, matching PC1500Keyboard::Key::Cl (the
        /// keycap is labeled "CL" on both models). The KEYDIRECT code
        /// table (TRM §10.2) has "CL" (code 18H) at this matrix position.
        Cl,
        Rcl, Enter, Space,
        /// RSV (RESERVE) -- the physical rocker-switch key at KS1 bit7,
        /// positioned between SML and RCL on the real keyboard, drawn with
        /// an up/down-arrow (rocker) glyph -- distinct from the separate
        /// Up/Down arrow keys (a separate matrix position), and the same
        /// distinct switch as PC1500Keyboard::Key::Rsv. Cycles among the
        /// three RESERVE memory areas. Its matrix position (KS1 bit7) is
        /// pinned down by (1) the ROM's own key-code table (KYCDTB, bank 6
        /// @ 94FBH in PC1600-P2-B6-new.bin): this exact matrix slot's byte is
        /// 09H, the KEYDIRECT code for this key's `⇕` keycap glyph per the
        /// TRM §10.2 key-code figure, and (2) the PC-1600 schematic's
        /// keyboard-matrix diagram, where this position is drawn with that
        /// same glyph.
        Rsv,
        F1, F2, F3, F4, F5, F6,
        Ctrl, Kbii, Bs,
        Unknown,
    };

    /// Maps a key name ("0".."9", "a".."z", "up"/"down"/
    /// "left"/"right", "shift", "sml", "mode", "def", "off", "cl", "rcl",
    /// "enter"/"ent", "space", "rsv", "f1".."f6", "ctrl", "kbii",
    /// "bs", "+", "-", "=", "*", "/", "(", ")", ".") to a Key. Returns
    /// Key::Unknown for anything unrecognized (including "on", which has no
    /// matrix position -- see class comment).
    static Key keyFromName(const std::string& name);

    void setKeyState(Key key, bool pressed);

    /// Lets go of every key in the matrix. Called on reset so a key the
    /// host lost track of (its release never arrived) can't outlive it.
    void releaseAll() { m_keys = {}; }

    /// Strobes the matrix: `opaDriveLines` is the KS0-KS7 column-select byte
    /// as written to OPA (port 1EH, active-low: bit n=0 selects KSn);
    /// `pb6Active` is OPB bit 6 (active-low: false/pulled-low = strobed --
    /// callers pass the already-inverted "is this strobe active" boolean,
    /// not the raw bit, since PB6 is a single line not a byte). Returns the
    /// port-37H sense byte, active-low the same way -- bit b is 0 if a
    /// pressed key shares sense bit b with any currently-strobed line
    /// (KS0-KS7 and/or PB6).
    uint8_t scan(uint8_t opaDriveLines, bool pb6Active) const;

    /// Total matrix strobes since construction -- bumped on every `scan()`
    /// (i.e. every I/O port 37H sense read). Not hardware; a headless probe
    /// for "the ROM's keyboard-poll loop is running" (it sweeps the whole
    /// matrix every idle-loop pass, so this races ahead once that loop
    /// starts, and barely moves while the ROM is still in boot / printer
    /// init). See PC1600PresetLoader's settle-before-keys logic.
    uint64_t scanCount() const { return m_scanCount; }

private:
    static constexpr int kStrobes = 9; // KS0-KS7 + PB6
    static constexpr int kSenseBits = 8;
    std::array<std::array<bool, kSenseBits>, kStrobes> m_keys{};
    mutable uint64_t m_scanCount = 0;
};
