#pragma once
#include <cstdint>

// ── uPD1990AC real-time clock ───────────────────────────────────────────
//
// A third chip beyond the LH5801 CPU's own internal timer and the LH5811
// I/O controller, bit-banged via OPC's PC0-PC5 lines and read back via
// PB5 (TP)/PB6 (DATA OUT) -- see PC1500Memory.hpp's I/O-chip comment for
// the surrounding register map. This models both features the ROM uses:
//
//  - TP's configurable-rate tick output and its edge-latch into IF bit 1
//    (BASIC's WAIT/BEEP timing) -- see the long block comment further down.
//  - The 40-bit BCD calendar shift register behind BASIC's TIME / TIME=
//    (ROM routines TIME2ARX $E5B4 and ARX_2_TIME $E59A, driven through
//    WRITE_2_CLOCK $E52B and the C0/C1/C2 mode-select at TIMER_MODE $E573).
//    C0-C2, latched on STB's rising edge, pick one of Register Hold /
//    Register Shift / Time Set / Time Read (C2=0), or a TP rate (C2=1).
//    In Register Shift / Time Set each CLK rising edge shifts the 40-bit
//    register right one bit: bit 0 falls out to DATA OUT (PB6), a fresh
//    bit from DATA IN (PC0) enters at bit 39. Time Read snapshots the
//    live running clock into the register; leaving Time Set commits the
//    shifted-in value back to the live clock. Field layout (LSB first,
//    the order WRITE_2_CLOCK walks RAM $7A06 downward): bits[3:0] =
//    seconds units ... bits[31:28] = day tens, bits[35:32] = day-of-week
//    (0-6, plain), bits[39:36] = month (1-12, plain). This Core is
//    cycle-driven, so the live clock advances via a 1 Hz accumulator in
//    advance() (see tickOneSecond()) rather than off the host wall-clock.
//    The PC-1500 boot ROM never touches the RTC on reset (the only
//    callers of the clock routines are the TIME / TIME= BASIC commands),
//    so -- unlike the PC-1600 -- there is no cold-start calendar re-init
//    to guard against; a one-time host seed via seedFromHost() just rides
//    through boot untouched.
//
// TP facts below (PC-pin mapping and command encoding) are confirmed
// against this project's own ROM disassembly: WAIT's poll loop at
// 0xE89C-0xE8BC calls vectored routine 0xA6, which resolves via this
// ROM's own 0xFFA6/0xFFA7 vector table to 0xE451 -- a "bii #(0xF00B),
// 0x02 ; rtn" IF-bit-1 test.
//
// This Core is cycle-driven rather than wall-clock-driven (advance() is
// called with each instruction's own cycle cost, see
// PC1500Machine::step()), which introduces a genuine race between OPB's
// read and IF's read: WAIT's poll loop checks OPB, then (a handful of
// instructions later, via the E451 helper) checks IF -- and advance() is
// still called once per instruction *between* those two reads, so a
// genuinely elapsed handful of cycles sits between them even in a fully
// deterministic model. Once IF's byte latches TP's edge, nothing in
// WAIT's own loop ever explicitly clears it (see PC1500Memory.hpp's
// m_if comment -- intentionally sticky), so if IF's read disagrees with
// OPB's immediately-preceding read even once, that stale disagreement
// persists and gets misread as BREAK on every later iteration of the
// *same* poll loop, not just the one where it happened -- surfacing as
// "BREAK AT <line>" firing within the first tick or two of any WAIT.
// Debounced by keeping IF's read in sync with OPB's own most-recent-read
// timestamp (kTpResyncDebounceSeconds, expressed here as elapsed
// cycle-time rather than wall-clock time), so a read landing shortly
// after OPB's own check reuses what OPB just established instead of
// independently re-deriving a different answer.
class Upd1990ac {
public:
    /// Forward every OPC write here with its six control-line levels
    /// (bit0=DATA IN/PC0, bit1=STB/PC1, bit2=CLK/PC2, bit3=C0, bit4=C1,
    /// bit5=C2). Command latching happens on STB's rising edge and the
    /// shift-register shift on CLK's rising edge, both detected
    /// internally, so callers don't track edges themselves.
    void setControlPins(bool dataIn, bool stb, bool clk, bool c0, bool c1, bool c2);

    /// Advances this chip by `cycles` CPU cycles' worth of real time (at
    /// kCpuHz). Called once per CPU step from PC1500Machine::step(). Drives
    /// two independent things: the 1 Hz calendar accumulator (always), and
    /// -- once the ROM has issued a TP rate-select -- TP's own sub-second
    /// tick. TP's rate is defined in real time, not CPU cycles, but this
    /// Core has no other notion of elapsed time to drive it from.
    void advance(uint32_t cycles);

    /// OPB's read (PB5, F00FH bit 5): the live TP level, OR'd with any
    /// not-yet-independently-observed pending edge so a read landing
    /// exactly on a transition still sees it high -- this is specifically
    /// what fixes WAIT's poll loop (0xE89C) missing individual TP edges.
    /// Consumes the pending-edge flag as a side effect, same as
    /// consumeRisingEdge() -- the two deliberately share that one flag,
    /// not two independent ones (see class doc comment).
    bool tp();

    /// OPB's read (PB6, F00FH bit 6): DATA OUT. In Register Shift / Time
    /// Set it is the shift register's LSB (what the ROM clocks out during
    /// a TIME read); otherwise a slow status square wave the ROM never
    /// actually samples.
    bool dataOut() const;

    /// IF's read (F00BH bit 1): true, and consumes the pending flag, if a
    /// TP rising edge has occurred since the last call to either this or
    /// tp(). Called independently of tp() by the ROM's own E451 helper,
    /// so IF alone can detect a fresh edge with no OPB read involved at
    /// all -- but see class doc comment for why this specifically
    /// debounces against tp()'s own last-read timestamp rather than
    /// always independently resyncing.
    bool consumeRisingEdge();

    /// Seed the live calendar from the host (see PC1500Machine::seedClock).
    /// month is 1-12, dow is 0-6 (Sunday=0); the rest are plain decimals.
    /// A deliberate convenience -- a real PC-1500's uPD1990AC is
    /// battery-backed and comes up already reading the right time; the
    /// boot ROM never re-inits it, so this one seed survives boot and the
    /// user is spared keying in the date/time on every launch.
    void seedFromHost(int year, int month, int day, int hour, int minute, int second, int dow);

private:
    void syncTp();
    void latchCommand(bool c0, bool c1, bool c2);
    void tickOneSecond();
    uint64_t liveTimeAsBcd40() const;
    void commitShiftRegisterToTime();

    // ~2.6MHz crystal / 2 -- matches the app layer's own real-time pacing
    // assumption, and PC1500BasicTyper.cpp's own kCpuHz.
    static constexpr double kCpuHz = 1300000.0;

    // The measured OPB-to-IF gap (~61us at 1.3MHz, via a vmj plus the
    // E451 helper's own bii+rtn) is ~79 cycles; 100us (~130 cycles)
    // comfortably covers that with margin while staying well under TP's
    // own half-period even at its fastest configured rate (2048Hz,
    // ~244us) -- see class doc comment.
    static constexpr double kTpResyncDebounceSeconds = 0.0001;

    // ── Calendar shift register + live clock ───────────────────────────
    enum class Mode { RegisterHold, RegisterShift, TimeSet, TimeRead };
    Mode mode_ = Mode::RegisterHold;

    bool prevStb_ = false;
    bool prevClk_ = false;
    bool dataIn_ = false;

    // 40 bits used -- see the class doc comment for the field layout.
    // Shifts right one bit per CLK rising edge while mode_ is
    // RegisterShift or TimeSet; bit 0 -> DATA OUT, DATA IN -> bit 39.
    uint64_t shiftRegister_ = 0;

    // Live running clock. sec/min/hour/day are one packed-BCD byte each;
    // month is plain 1-12, dow plain 0-6 (Sunday=0). year_ is internal
    // only (the chip has no year field) -- kept for February's length.
    // Defaults are a fixed, arbitrary instant so a fresh (unseeded) chip
    // is deterministic for headless tests; the app seeds real time via
    // seedFromHost().
    uint8_t clkSec_ = 0x00;
    uint8_t clkMin_ = 0x00;
    uint8_t clkHour_ = 0x00;
    uint8_t clkDay_ = 0x01;
    uint8_t clkDow_ = 0x06;  // 2000-01-01 was a Saturday
    uint8_t clkMonth_ = 1;
    int clkYear_ = 2000;

    // 1 Hz accumulator driving tickOneSecond() -- fed by advance() every
    // instruction, off emulated cycles, never gated by tpConfigured_.
    double rtcAccumSeconds_ = 0.0;
    // Monotonic total elapsed seconds, only for dataOut()'s status
    // waveform in the non-shift modes (the ROM never samples it there).
    double rtcElapsedSeconds_ = 0.0;

    // ── TP output ─────────────────────────────────────────────────────
    bool tpConfigured_ = false; // see latchCommand's own comment
    int tpRateHz_ = 64;
    double elapsedSeconds_ = 0.0; // since TP was last (re-)configured
    bool tpLevel_ = false;
    bool tpEdgePending_ = false;

    // Records elapsedSeconds_ at tp()'s (OPB's) most recent read -- see
    // consumeRisingEdge()'s debounce. Starts "never synced" so a BREAK
    // check (which reads IF directly, never through OPB -- see class doc
    // comment) always gets a genuine fresh resample rather than trusting
    // a stale or nonexistent OPB timestamp.
    double lastOpbSyncSeconds_ = 0.0;
    bool everOpbSynced_ = false;
};
