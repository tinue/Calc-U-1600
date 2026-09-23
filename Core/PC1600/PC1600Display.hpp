#pragma once
#include <array>
#include <cstdint>

#include "PC1600StatusLine.hpp"

// ── PC-1600 LCD (1x HD61203 + 2x HD61102) ────────────────────────────────
//
// Model of the LF7204E panel per
// SharpPC1500Reference/PC-1600/PC-1600-Display-HD61202.md: 156x32 graphics
// dots split across two HD61102 column-driver chips (IC2/IC3), standard
// HD61102 register model (command vs. data selected by which port-number
// offset within a 4-port block is addressed, rather than a dedicated D/I
// pin -- see the decode below).
//
// **CS1#/CS2#/CS3 decode** per the Technical Reference Manual's pinout
// table (Systemhandbuch §7.3): IC2 selects on A2=0/A5=0/A4=1 (50H-53H and
// 58H-5BH), IC3 on A3=0/A5=0/A4=1 (50H-53H and 54H-57H) -- both
// chip-select simultaneously only within 50H-53H (where A2=A3=0), giving
// the documented "50H-53H = both controllers, 54H-57H = IC3 only,
// 58H-5BH = IC2 only" split. **Command/data/status/read decode within
// each 4-port block** (see writeIO's own comment for the full story):
// offset 0 = command write, offset 2 = data write, offset 1 = status
// read, offset 3 = data read.
//
// **The 64+64+28-dot three-block column split** comes from the TRM's own
// LCD block diagram (Systemhandbuch): the right 28-dot block reads the
// SAME per-column byte array as the left block but from **pages 4-7
// instead of pages 0-3** of that same chip -- genuinely independent,
// individually-addressable pixels (an HD61102 here has 8 pages per
// column, i.e. 64 raw rows of storage, more than the 32-row window
// actually shown for the left/centre blocks; the right block exposes the
// otherwise-hidden upper half of that same storage). Real hardware's
// `displaySL` (0xC0-0xFF, "set display start line") rotates that window
// for all three blocks alike. (The diagram also shows the status-symbol
// line fed from a distinct source -- IC3's `Y6f` pin and HD61203's
// `X49-X64` common lines, both outside the main screen's own
// Y1-Y64/X1-X32 addressing -- consistent with `PC1600StatusLine` being a
// genuinely separate mechanism; see that class's own comment.)
class PC1600Display {
public:
    static constexpr int kWidth = 156;
    static constexpr int kHeight = 32;
    static constexpr int kRightBlockColumnStart = 128; // see class comment
    // Raw-row offset the right block reads at relative to the left/centre
    // blocks, before the shared `displaySL` rotation is applied (see
    // `readPixel()`) -- 32, i.e. "the other half of the chip's 64 raw
    // rows." Not a page number by itself once `displaySL` is nonzero;
    // kept as a named constant for what it means at `displaySL == 0`
    // (pages 4-7), the default/reset state.
    static constexpr int kRightBlockRowShift = 32;

    /// `port` is the low 8 bits of the I/O address (50H-5BH is the only
    /// range this class claims -- callers should only forward addresses in
    /// that range). Status reads (port's bit0 clear) return the busy flag
    /// (bit7) for kBusyClocks LCD-clock edges after each write; the boot
    /// ROM and user code busy-wait on it, see the .cpp's own comment. Data
    /// reads (bit0 set)
    /// return the byte at the controller's current (column, page) address,
    /// auto-incrementing the column the same way a data write does.
    uint8_t readIO(uint8_t port);
    void    writeIO(uint8_t port, uint8_t value);

    /// How long a controller reports busy (status bit 7) after a command or
    /// data write: until the kBusyClocks-th edge of its own LCD clock
    /// (phi-OS 1.3 MHz / 6 = 216.7 kHz, ~16.5 SC-7852 T-states per edge),
    /// which free-runs asynchronously to the CPU. See the .cpp's readIO().
    static constexpr int kBusyClocks = 4;
    /// Credits elapsed SC-7852 T-states to the LCD clock.
    void tick(int tstates) {
        m_lcdClockAccum += static_cast<int64_t>(tstates) * kLcdClockHzTimes6;
        while (m_lcdClockAccum >= kTStateHzTimes6) {
            m_lcdClockAccum -= kTStateHzTimes6;
            ++m_lcdEdges;
        }
    }

    /// True while CK0 (LCD base clock) is enabled -- Z-80 I/O port 37H bit
    /// 4, see PC1600Memory's I/O decode. The display renders as blank
    /// while this is false, matching real power-on behavior.
    void setClockEnabled(bool enabled) { m_clockEnabled = enabled; }
    bool clockEnabled() const { return m_clockEnabled; }

    /// Direct pixel readout for rendering (not part of the hardware
    /// interface): true = pixel on. Out-of-range (x,y) always returns
    /// false; a column at or past `kRightBlockColumnStart` (128-155)
    /// reads IC2's own column `x - kRightBlockColumnStart` (0-27), offset
    /// by `kRightBlockRowShift` raw rows and the controller's own
    /// `displaySL` scroll register -- see the class comment and
    /// `readPixel()`'s own. A controller whose display is currently off
    /// also reads false.
    bool pixel(int x, int y) const;

    /// The fixed-legend status-symbol strip above the graphics area,
    /// wired to real display memory -- see `refreshStatusSymbols()` and
    /// `PC1600StatusLine`'s own class comment for the TRM bit map and
    /// storage location.
    PC1600StatusLine&       statusLine() { return m_statusLine; }
    const PC1600StatusLine& statusLine() const { return m_statusLine; }

    /// Recomputes every `statusLine()` flag from IC3's own column 63,
    /// pages 4/6/7 (rotated by IC3's own `addressStartLine`, same as the
    /// graphics-area scroll) per the TRM's SMBLSET bit map -- see
    /// `PC1600StatusLine.hpp`'s own class comment for the full
    /// derivation. Called after every write that could have
    /// touched that cell (`writeIO()`, both for IC3's own data/command
    /// ports and its `addressStartLine` register, since a scroll rotates
    /// which page the same three positions read from) -- idempotent and
    /// cheap enough to just always recompute rather than track precisely
    /// which write mattered. While IC3's display is off (a 0x3E command --
    /// e.g. the ROM's power-down path), every symbol reads back off, the
    /// same way `readPixel()` gates the graphics area on `displayOn`.
    void refreshStatusSymbols();

private:
    // Standard HD61102 geometry: 64 columns x 8 pages x 8 bits/page --
    // 64 "raw rows" total (page*8+bit), of which only 32 are ever shown
    // on the left/centre blocks at once; `addressStartLine` (0-63)
    // selects which 32-row window, exactly like a real HD61102's display
    // start line register. This rotation must be applied on every read --
    // a fixed always-0 window would draw ROM scroll behavior (command
    // 0xC0-0xFF) into the wrong apparent screen position.
    struct Controller {
        std::array<std::array<uint8_t, 8>, 64> pages{}; // [column][page]
        uint8_t addressCol{0};       // current column address (0-63), auto-increments on data write
        uint8_t addressPage{0};      // current page (0-7) -- direct register access, NOT windowed by addressStartLine (see readIO's own dataByte)
        uint8_t addressStartLine{0}; // display start line (0-63), command 0xC0-0xFF
        bool displayOn{false};
        uint64_t busyUntilEdge{0};   // status bit 7 stays set until m_lcdEdges reaches this (see kBusyClocks)
    };

    void writeCommand(Controller& c, uint8_t value);
    /// True if the pixel at raw-row-space `y + rowShift`, rotated by the
    /// controller's own `addressStartLine`, is set -- `rowShift` is 0 for
    /// the left/centre blocks, `kRightBlockRowShift` (32) for the right
    /// block (see class comment). Goes from a wanted visible `y` back to
    /// which raw (page, bit) to read, since that's the direction this
    /// class's callers need.
    bool readPixel(const Controller& c, int col, int y, int rowShift) const;

    Controller m_ic2; // panel columns 0-63
    // LCD clock: phi-OS (1.3 MHz) / 6, counted in edges. Scaled by 6 so the
    // accumulator stays integral: SC-7852 T-states * 1.3 MHz vs 3.58 MHz * 6.
    static constexpr int64_t kLcdClockHzTimes6 = 1300000;
    static constexpr int64_t kTStateHzTimes6 = 3580000LL * 6;
    int64_t  m_lcdClockAccum{0};
    uint64_t m_lcdEdges{0};
    Controller m_ic3; // panel columns 64-127
    bool m_clockEnabled{false};
    PC1600StatusLine m_statusLine;
};
