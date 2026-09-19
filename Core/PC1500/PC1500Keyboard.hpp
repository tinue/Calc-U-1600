#pragma once
#include <array>
#include <cstdint>
#include <string>

// ── PC-1500/1500A physical keyboard matrix ───────────────────────────────
//
// 8 (column strobe, PA0-PA7) x 8 (row read, IN0-IN7) matrix, confirmed by
// live hardware testing on a real PC-1500.
//
// Wiring (per the hardware reference doc): columns are driven by the
// LH5811 I/O-port controller's OPA register (a memory-mapped ME1 store —
// see PC1500Memory's I/O-chip decode); rows are read directly off the
// CPU's IN0-IN7 pins via the LH5801's ITA instruction, bypassing the I/O
// controller entirely. Both directions are active-low: a driven column has
// its bit 0 in the strobe byte, and a pressed key pulls its row bit to 0
// in the read-back byte.
//
// The ON key is deliberately NOT part of this matrix — real hardware wires
// it straight to the CPU's BFI pin (a power-on latch), not through the
// column/row grid. Modeling that wake path is out of scope for now (see
// PC1500Machine's doc comment); pressKey("on") is a no-op here.
class PC1500Keyboard {
public:
    // Named-key vocabulary used by preset scripts and the BASIC typer.
    enum class Key {
        Digit0, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,
        A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
        Plus, Minus, Equals, Asterisk, Slash, LeftParen, RightParen, Period,
        Left, Right, Up, Down,
        Shift, Sml, Mode, Def, Off, Cl, Rcl, Ent, Space,
        F1, F2, F3, F4, F5, F6,
        /// RSV (RESERVE) -- the physical rocker-switch key at IN7/PA1,
        /// positioned between SML and RCL on the real keyboard, drawn with
        /// an up/down-arrow (rocker) glyph -- distinct from the plain
        /// Up/Down arrow keys (a separate matrix position). Cycles among
        /// the three RESERVE memory areas. Electrically a single matrix
        /// position (this class's kMatrix has only one slot here, not
        /// two), so it's modeled as one key.
        Rsv,
        Unknown,
    };

    /// Maps a key name ("cl", "enter"/"ent", "a".."z", "0".."9",
    /// "f1".."f6", "up"/"down"/"left"/"right", "rsv", "space", "shift",
    /// "mode", "def", "sml", "rcl", "off", "+", "-", "=", "*", "/", "(",
    /// ")", ".") to a Key. Returns Key::Unknown for anything unrecognized
    /// (including "on" and "shift+..." prefixes, which callers handle
    /// separately).
    static Key keyFromName(const std::string& name);

    void setKeyState(Key key, bool pressed);
    void setKeyState(int row, int col, bool pressed); // row=IN0-7, col=PA0-7

    /// Lets go of every key in the matrix. Called on reset so a key the
    /// host lost track of (its release never arrived) can't outlive it.
    void releaseAll() { m_keys = {}; }

    /// Strobes the matrix: driveLines is the column-select byte as written
    /// to the LH5811's OPA register (active-low: bit c=0 selects column c).
    /// Returns the row-read byte, active-low the same way — bit r is 0 if
    /// a pressed key shares row r with any currently-strobed column.
    uint8_t scan(uint8_t driveLines) const;

private:
    static constexpr int kRows = 8;
    static constexpr int kCols = 8;
    std::array<std::array<bool, kCols>, kRows> m_keys{};
};
