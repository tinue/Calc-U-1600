#pragma once
#include <array>
#include <cstddef>

// ── PC-1600 LCD status-symbol line ───────────────────────────────────────
//
// The 16-symbol strip above the 156x32 graphics area (TRM §7.3;
// SharpPC1500Reference/PC-1600/PC-1600-Display-HD61202.md §1). These are
// printed fixed-legend text permanently etched on the LCD glass, each
// with its own small individually-drivable segment -- the same style as a
// scientific calculator's fixed DEG/RAD/GRAD strip. This class is
// therefore a plain on/off flag per named position, not a font/glyph
// render the way PC1600Display's 156x32 dot-matrix area is.
//
// **Bit map**, from the TRM's own SMBLSET page (entry 013CH).
// SMBLSET(A,B): B = symbol-SET number 0-2, A = an 8-bit mask ("1 = ON")
// applied to that set's 8 positions, MSB to LSB:
//
//   B=00H: DEF   I     II    III   SMALL  --    SHIFT  BUSY
//   B=01H: --    RUN   PRO   RESERVE --   RAD   G(rad) DE(g)
//   B=02H: KBII  --    --    --    S     --     CTRL   BATT
//
// (`--` = unused/undefined in the TRM's own table.) The TRM's own note:
// *"Wenn entweder KBII oder S auf 1 gesetzt ist, so wird das S Symbol
// angezeigt"* -- "if either KBII or S is set to 1, the S symbol is
// shown". That describes what `SMBLSET` does with its *argument*, not the
// panel's wiring: the firmware (PC1600-P2-B6.bin 822DH-8239H) masks bits 3 and
// 7 out of the caller's byte (`AND 77H`) and lights bit 3 -- S -- alone
// (`OR 08H`) if either was set, so the byte reaching the glass can never
// carry bit 7 with this ROM.
//
// **`Kbii` is nevertheless a real, independently-driven segment**: a
// second printed position next to "S" carries the "ローマ字→カナ"
// (romaji→kana) caption. With the contrast cranked up far enough to
// reveal unlit segments, "カナ" is visible alongside "S" even on a
// **western** unit -- i.e. same glass, same segment wiring on both
// models, and which of the two lights is a ROM decision. A Japanese
// machine's firmware is assumed to drive bit 7 for kana-entry mode where
// this one folds into S.
//
// So `Kbii` is read straight off panel bit 7 like every other position
// (`PC1600Display::refreshStatusSymbols()`), and simply stays false for
// the life of a western session. It must NOT be wired to the KBII *mode*
// flag (RAM F3C6H bit 7, SMBLSET's own pre-fold shadow, which is what the
// key-code translator reads to select the alternate charset): that is
// machine state rather than a panel segment, and driving the caption from
// it would light the kana legend on every KBII press.
//
// **DEGRAD and RUNPRO — how they pack into that grid**, consistent with
// the TRM's own three-separate-bits DE/RAD/G row above:
//   - **DEGRAD is one physical legend showing whichever one of
//     "DEG"/"RAD"/"GRAD" applies** -- not three simultaneously-lit
//     segments (though the TRM models them as three independent bits;
//     real firmware is assumed to only ever set one at a time, same as
//     any other angle-mode indicator). One shared start position; the
//     GUI picks the matching text (see `PC1600LCDDisplayView.swift`).
//   - **RUNPRO is two independently-driven legends** that simply sit
//     close enough together to visually read as one word.
//
// **Wiring**: these three 8-bit sets are read from the *centre* HD61102
// controller's own column 63, pages 4/6/7 respectively, rotated by the
// same `addressStartLine` the display-scroll logic uses --
// page4=B=02H, page6=B=01H, page7=B=00H. A headless trace independently
// found non-zero data at exactly IC3 (the centre chip)'s column 63,
// pages 6/7 during boot, consistent with this being the real mechanism.
// See `PC1600Display::refreshStatusSymbols()`.
class PC1600StatusLine {
public:
    enum class Symbol {
        Busy, Shift, S, Kbii, Small, Deg, Rad, Grad, Run, Pro, Reserve, Def,
        I, II, III, Ctrl, Batt,
        Count
    };
    static constexpr size_t kCount = static_cast<size_t>(Symbol::Count);

    bool isOn(Symbol s) const { return m_state[static_cast<size_t>(s)]; }
    void set(Symbol s, bool on) { m_state[static_cast<size_t>(s)] = on; }

    /// Every position's current state, in `Symbol` declaration order --
    /// the order the GUI renders them left to right, matching the photo.
    const std::array<bool, kCount>& all() const { return m_state; }

    void reset() { m_state.fill(false); }

private:
    std::array<bool, kCount> m_state{};
};
