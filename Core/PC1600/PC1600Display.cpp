#include "PC1600Display.hpp"

// HD61102 command byte encoding (not detailed in
// PC-1600-Display-HD61202.md, which only describes command functionality,
// not exact bit patterns -- see that doc's §3):
//   0x3E           display off (bit0 = 0)
//   0x3F           display on  (bit0 = 1)
//   0xB8-0xBF      set page address (low 3 bits = page 0-7)
//   0x40-0x7F      set column address (low 6 bits = column 0-63)
//   0xC0-0xFF      set display start line (low 6 bits) -- see
//                  Controller's own comment and readPixel() below
namespace {
constexpr uint8_t kCmdDisplayOff = 0x3E;
constexpr uint8_t kCmdDisplayOn = 0x3F;
}

void PC1600Display::writeCommand(Controller& c, uint8_t value) {
    if (value == kCmdDisplayOff) { c.displayOn = false; return; }
    if (value == kCmdDisplayOn) { c.displayOn = true; return; }
    if (value >= 0xB8 && value <= 0xBF) { c.addressPage = value & 0x07; return; }
    if (value >= 0x40 && value <= 0x7F) { c.addressCol = value & 0x3F; return; }
    if (value >= 0xC0) { c.addressStartLine = value & 0x3F; return; }
}

uint8_t PC1600Display::readIO(uint8_t port) {
    // The ROM busy-waits on a status read from both controllers before
    // every access (`IN A,(59H)` / `IN A,(55H)` at 0x0807/0x080A in
    // PC1600-P0-B0-new.bin; user code like dampflok.bas's &D000 routine
    // does the same). Busy (bit7) is set by each command/data write and
    // clears on the kBusyClocks-th edge of the controllers' own 216.7 kHz
    // clock. That clock runs asynchronously to the CPU, so whether a poll
    // ~60 T after a write still sees busy depends on the phase.
    //
    // Fitted 2026-09-23 to a real unit: a BEEP-bracketed loop of 100
    // scrolling PRINTs is 3.9% slower on hardware than with busy always
    // clear, 2.7% after the sub-CPU latency and the uniform ~0.65% residual
    // left by every BASIC benchmark. 3 edges changes nothing, 4 edges gives
    // 5.854 s (real 5.894 s, i.e. the residual), 5 edges overshoots to
    // 6.257 s. Dampflok.bas drops from 1.8% to 0.87% slow with it. This is
    // a fit, not a datasheet figure, but it lies inside the datasheet's
    // bound of 1/fCLK <= T_BUSY <= 3/fCLK: the 216.7 kHz clock is CK0,
    // the HD61203's oscillator input, and the HD61203 divides it by two
    // into phi1/phi2, so fCLK = 108.3 kHz (9.2-27.7 us). 3-4 CK0 periods
    // are 13.8-18.5 us, about 2 phi cycles.
    //
    // Per-block offset (see class comment / writeIO's own comment for the
    // full derivation): offset 1 = status read, offset 3 = data read (the
    // ROM's data reads at bank 6 `81E8`/`8AA2` use 57H/5BH).
    // Offsets 0/2 (the write-side command/data ports) have no defined
    // read meaning and fall through to the "both, default IC2" case
    // below, same as an unhandled port.
    uint8_t offset = port & 0x03;

    // Status byte (HD61102 datasheet, Hitachi 1989): DB7 = busy (see
    // above), DB5 = ON/OFF with 1 = display *off* (the reverse of the
    // instruction's D bit), DB4 = RESET. DB4 stays clear: no RST line is
    // modelled, and the controllers are deliberately not reset on power-on
    // (VGG keeps their RAM and registers). The ROM's busy-wait masks bit 7
    // of each chip (`817F`: IN A,(59H) / RLA / IN A,(55H) / RRA / AND C0H).
    auto statusByte = [this](const Controller& c) -> uint8_t {
        uint8_t status = c.displayOn ? 0x00 : 0x20;
        if (lcdEdges() < c.busyUntilEdge) status |= 0x80;
        return status;
    };
    // Data byte: a read returns the controller's output register, which
    // the previous read loaded. The read then latches the RAM cell at the
    // current address into it and increments the column (datasheet Fig. 5).
    // So the first read after setting an address returns stale data and
    // the addressed byte comes with the second. The ROM does that dummy
    // read (`81E8`: IN A,(C) discarded) before its real reads (`8AA2`), and
    // its cursor-blink read-modify-write depends on it.
    auto dataByte = [](Controller& c) -> uint8_t {
        uint8_t v = c.outputReg;
        c.outputReg = c.pages[c.addressCol][c.addressPage];
        c.addressCol = static_cast<uint8_t>((c.addressCol + 1) & 0x3F);
        return v;
    };

    if (port >= 0x54 && port <= 0x57) return (offset == 3) ? dataByte(m_ic3) : statusByte(m_ic3);
    if (port >= 0x58 && port <= 0x5B) return (offset == 3) ? dataByte(m_ic2) : statusByte(m_ic2);
    // 50H-53H ("both controllers" on the write side): no clean single-chip
    // answer for a read from an ambiguous shared address -- defaults to
    // IC2, consistent with 58H-5BH's own IC2-only mapping.
    return (offset == 3) ? dataByte(m_ic2) : statusByte(m_ic2);
}

void PC1600Display::writeIO(uint8_t port, uint8_t value) {
    // Per-block port offset: within each 4-port block (both=50-53,
    // IC3=54-57, IC2=58-5B), offset 0 is the command-byte write, offset 2
    // is the data-byte write; offset 1/3 are read-only (status/data
    // respectively, see readIO). Real boot code only ever writes to
    // offset-0/2 ports (50/52/54/56/58/5A), issuing page/column-address
    // commands (0xB8-0xBF, 0x40-0x7F) immediately followed by pixel data
    // at the offset-2 port right after -- and reads only ever target
    // offset-1 ports (55H/59H, the busy-wait).
    uint8_t offset = port & 0x03;
    bool isData = (offset == 2);

    auto apply = [&](Controller& c) {
        if (offset == 0 || isData) c.busyUntilEdge = lcdEdges() + kBusyClocks;
        if (!isData) {
            if (offset == 0) writeCommand(c, value);
            return; // offset 1/3: read-only, no write-side meaning
        }
        if (c.addressCol < 64) {
            c.pages[c.addressCol][c.addressPage] = value;
            c.addressCol = static_cast<uint8_t>((c.addressCol + 1) & 0x3F);
        }
    };

    if (port >= 0x50 && port <= 0x53) { apply(m_ic2); apply(m_ic3); refreshStatusSymbols(); return; }
    if (port >= 0x54 && port <= 0x57) { apply(m_ic3); refreshStatusSymbols(); return; }
    if (port >= 0x58 && port <= 0x5B) { apply(m_ic2); return; }
}

bool PC1600Display::readPixel(const Controller& c, int col, int y, int rowShift) const {
    if (!c.displayOn || col < 0 || col >= 64) return false;
    // Goes from a wanted visible y to which raw (page,bit) address to
    // read -- the inverse of the controller's own mapping from a raw
    // address to a visible y (subtract displaySL and wrap). See the class
    // comment on Controller for why this rotation must be applied.
    int rawRow = (y + rowShift + c.addressStartLine) & 0x3F; // mod 64
    int page = rawRow >> 3;
    int bit = rawRow & 7;
    uint8_t byte = c.pages[static_cast<size_t>(col)][static_cast<size_t>(page)];
    return (byte & (1 << bit)) != 0;
}

bool PC1600Display::pixel(int x, int y) const {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return false;
    // Columns 128-155 are IC2's own upper raw rows (32-63 before the
    // addressStartLine rotation -- pages 4-7 when addressStartLine == 0,
    // the default/reset state), columns 0-27 of that same chip -- genuinely
    // independent, individually-addressable memory, NOT a duplicate/mirror
    // of columns 0-27's own lower rows.
    if (x >= kRightBlockColumnStart) {
        return readPixel(m_ic2, x - kRightBlockColumnStart, y, kRightBlockRowShift);
    }
    const Controller& c = (x < 64) ? m_ic2 : m_ic3;
    int col = (x < 64) ? x : (x - 64);
    return readPixel(c, col, y, 0);
}

void PC1600Display::refreshStatusSymbols() {
    using Symbol = PC1600StatusLine::Symbol;

    // The status-symbol segments hang off IC3 (its Y6f pin, per
    // PC-1600-Display-HD61202.md §2), so a "display off" command (0x3E) to
    // IC3 stops them being driven -- exactly as it stops the graphics area
    // (see readPixel()'s own `!c.displayOn` guard). Without this, the ROM's
    // auto-power-off / OFF-key power-down would blank the 156x32 dot-matrix
    // but leave DEG/RUN/BUSY etc. frozen on the glass. The pixel RAM behind
    // these cells is retained across the off state (VGG rail), so a later
    // 0x3F just recomputes from whatever was there -- same as the graphics
    // area.
    if (!m_ic3.displayOn) { m_statusLine.reset(); return; }

    // Same rotation as readPixel()'s own displaySL handling, but at page
    // granularity only (symbol storage is addressed by whole page, not
    // by individual raw row).
    auto symbolPage = [&](int basePage) {
        int p = basePage + (m_ic3.addressStartLine >> 3);
        return (p >= 8) ? p - 8 : p;
    };
    auto bit = [](uint8_t byte, int b) { return (byte & (1 << b)) != 0; };

    // TRM SMBLSET table (see PC1600StatusLine.hpp's own class comment):
    // B=00H -> IC3 page 7, B=01H -> page 6, B=02H -> page 4.
    uint8_t b00 = m_ic3.pages[63][static_cast<size_t>(symbolPage(7))];
    uint8_t b01 = m_ic3.pages[63][static_cast<size_t>(symbolPage(6))];
    uint8_t b02 = m_ic3.pages[63][static_cast<size_t>(symbolPage(4))];

    m_statusLine.set(Symbol::Def,   bit(b00, 7));
    m_statusLine.set(Symbol::I,     bit(b00, 6));
    m_statusLine.set(Symbol::II,    bit(b00, 5));
    m_statusLine.set(Symbol::III,   bit(b00, 4));
    m_statusLine.set(Symbol::Small, bit(b00, 3));
    m_statusLine.set(Symbol::Shift, bit(b00, 1));
    m_statusLine.set(Symbol::Busy,  bit(b00, 0));

    m_statusLine.set(Symbol::Run,      bit(b01, 6));
    m_statusLine.set(Symbol::Pro,      bit(b01, 5));
    m_statusLine.set(Symbol::Reserve,  bit(b01, 4));
    m_statusLine.set(Symbol::Rad,      bit(b01, 2));
    m_statusLine.set(Symbol::Grad,     bit(b01, 1));
    m_statusLine.set(Symbol::Deg,      bit(b01, 0));

    // Kbii (the "カナ" romaji->kana legend) is a real, independently-driven
    // segment at b02 bit7 -- but one this ROM never lights, so on a western
    // machine it stays dark forever. That is faithful, not dead code.
    //
    // SMBLSET (bank 6, 822DH-8239H) implements the TRM's own "if either
    // KBII or S is set to 1, the S symbol is shown" rule in firmware: for
    // symbol set 2 it masks bits 3 and 7 out of the caller's byte
    // (`AND 77H`) and, if either was set, lights bit 3 -- S -- alone
    // (`OR 08H`). So the panel byte can never carry bit7 with this
    // firmware, and pressing KBII shows up as S.
    //
    // The glass itself carries both legends: with the contrast cranked up
    // far enough to reveal unlit segments, "カナ" is etched alongside "S"
    // even on a western unit. Same panel, same segment wiring; which of
    // the two lights is a ROM decision, and a Japanese machine's firmware
    // is assumed to drive bit7 for kana-entry mode where this one folds
    // into S. Reading the bit straight off the panel keeps both cases
    // right without special-casing either.
    //
    // (The KBII *mode* flag is a different thing living elsewhere -- RAM
    // F3C6H bit7, SMBLSET's own pre-fold shadow, which is what the key-code
    // translator reads to select the alternate charset table. It is machine
    // state, not a panel segment, so it is deliberately not surfaced here.)
    m_statusLine.set(Symbol::Kbii, bit(b02, 7));
    m_statusLine.set(Symbol::S,    bit(b02, 3));
    m_statusLine.set(Symbol::Ctrl, bit(b02, 1));
    m_statusLine.set(Symbol::Batt, bit(b02, 0));
}
