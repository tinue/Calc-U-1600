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
// panel's wiring: the firmware (PC1600-P2-B6-new.bin 822DH-8239H) masks bits 3 and
// 7 out of the caller's byte (`AND 77H`) and lights bit 3 -- S -- alone
// (`OR 08H`) if either was set, so the byte reaching the glass can never
// carry bit 7 with this ROM.
//
// **The "ローマ字→カナ" (romaji→kana) caption is two segments of its
// own, not KBII.** The Service Manual's key circuit diagram (printed
// pp. 43-44) lists the glass pin under each legend. Every symbol above sits
// on the common its bit predicts (common Xn = RAM line n-1), and the caption
// after S is on **X35 = page 4 bit 2** and **X59 = page 7 bit 2**, both "--"
// in the TRM table. KBII's own bit (page 4 bit 7 = X40) has no electrode,
// which fits the ROM folding it into S. `Romaji` and `Kana` follow the pin
// order after S. Which part of the caption each one lights is inferred, not
// printed. The western ROM never sets either bit (probe 2026-09-26), so both
// stay dark unless a program writes them. SMBLSET B=02H keeps bit 2
// (`AND 77H`).
//
// The KBII *mode* flag (RAM F3C6H bit 7, SMBLSET's pre-fold shadow, read
// by the key-code translator) is machine state, not a segment, and is not
// surfaced here.
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
// page4=B=02H, page6=B=01H, page7=B=00H. The ROM's symbol writer confirms
// the storage (PC1600-P2-B6-new.bin 8220H, called with the page in A): it
// sets page (A + DSPLPTR F05CH) & 7 and column 3FH on IC3 (C = 54H, via
// 81F0H/81FCH), then writes the byte with OUT (56H). Its RAM shadows
// (read by 8208H) are F64EH/F64FH/F3C6H for pages 7/6/4; page 5 is unused.
// See `PC1600Display::refreshStatusSymbols()`.
class PC1600StatusLine {
public:
    enum class Symbol {
        Busy, Shift, S, Romaji, Kana, Small, Deg, Rad, Grad, Run, Pro, Reserve, Def,
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
