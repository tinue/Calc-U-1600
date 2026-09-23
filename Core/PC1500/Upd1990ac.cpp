#include "Upd1990ac.hpp"

#include "../BcdCalendar.hpp"

void Upd1990ac::setControlPins(bool dataIn, bool stb, bool clk, bool c0, bool c1, bool c2) {
    dataIn_ = dataIn;

    if (stb && !prevStb_) latchCommand(c0, c1, c2);
    prevStb_ = stb;

    if (clk && !prevClk_ && (mode_ == Mode::RegisterShift || mode_ == Mode::TimeSet)) {
        shiftRegister_ = (shiftRegister_ >> 1) |
                         (static_cast<uint64_t>(dataIn_ ? 1 : 0) << 39);
    }
    prevClk_ = clk;
}

void Upd1990ac::latchCommand(bool c0, bool c1, bool c2) {
    int sel = (c1 ? 2 : 0) | (c0 ? 1 : 0);

    if (!c2) {
        // Group 0 (C2=0): pick one of the four register-control modes.
        Mode oldMode = mode_;
        switch (sel) {
            case 0: mode_ = Mode::RegisterHold;  break;
            case 1: mode_ = Mode::RegisterShift; break;
            case 2: mode_ = Mode::TimeSet;       break;
            case 3:
                mode_ = Mode::TimeRead;
                // Time Read snapshots the live running clock into the shift
                // register; it's the following Register Shift that walks it
                // out over DATA OUT via CLK pulses (see setControlPins()).
                shiftRegister_ = liveTimeAsBcd40();
                break;
            default: break;
        }
        // Time Set pauses the live clock while new nibbles shift in;
        // committing on exit (rather than bit-by-bit) is indistinguishable
        // to anything that only reads the result afterward, and simpler.
        if (oldMode == Mode::TimeSet && mode_ != Mode::TimeSet) commitShiftRegisterToTime();

        // Any Group 0 command also means WAIT/BEEP is done with TP:
        // WAIT/BEEP's own cleanup issues this ("TP=RegisterHold", the
        // ROM's own E8B4/E8C3 abort and successful-completion paths both
        // do) when done with TP. Un-gates TP entirely until the next
        // rate-select: without this, TP would keep ticking in the
        // background forever after the first WAIT/BEEP ever ran, and any
        // later, unrelated IF read (the idle loop's own BREAK check, a
        // statement-boundary break-check) would misread a genuine RTC
        // tick as BREAK.
        tpConfigured_ = false;
        elapsedSeconds_ = 0.0;
        tpLevel_ = false;
        tpEdgePending_ = false;
        everOpbSynced_ = false;
        return;
    }

    switch (sel) {
        case 0: tpRateHz_ = 64; tpConfigured_ = true; break;
        case 1: tpRateHz_ = 256; tpConfigured_ = true; break;
        case 2: tpRateHz_ = 2048; tpConfigured_ = true; break;
        default: break; // test mode -- not modeled
    }
    if (tpConfigured_) {
        // TP is tapped off the chip's free-running 32.768 kHz divider
        // chain, so a rate select doesn't restart its phase: it just
        // starts showing the divider's current level, and edges stay on
        // the 1/rate grid. A real unit's BEEP repeat periods are whole
        // 64ths of a second because of this (8/64 s, 9/64 s); restarting
        // the phase here made them 127.0 / 139.4 / 143.8 ms instead.
        elapsedSeconds_ = 0.0;
        tpLevel_ = tpLevelNow();
        tpEdgePending_ = false;
        // Force consumeRisingEdge() to do its own genuine fresh sample on
        // the next read rather than trusting a now-stale pre-(re-)configure
        // OPB timestamp.
        everOpbSynced_ = false;
    }
}

void Upd1990ac::advance(uint32_t cycles) {
    // The calendar clock free-runs independently of TP -- it has its own
    // divider off the same 32.768kHz crystal on real hardware -- so it
    // must advance whether or not the ROM has ever configured TP.
    double dt = static_cast<double>(cycles) / kCpuHz;
    rtcElapsedSeconds_ += dt;
    rtcAccumSeconds_ += dt;
    while (rtcAccumSeconds_ >= 1.0) {
        rtcAccumSeconds_ -= 1.0;
        tickOneSecond();
    }

    // TP produces no edges at all until the ROM has issued at least one
    // rate-select command: an always-on TP from power-on, using a guessed
    // default rate, would spuriously set IF bit 1 during the boot ROM's
    // own "NEW0?:CHECK" prompt sequence, well before WAIT/BEEP ever
    // configure it.
    if (!tpConfigured_) return;
    elapsedSeconds_ += dt;
}

void Upd1990ac::tickOneSecond() {
    if (!bcdBumpField(clkSec_, 59, 0)) return;
    if (!bcdBumpField(clkMin_, 59, 0)) return;
    if (!bcdBumpField(clkHour_, 23, 0)) return;
    // Day rolled -- also advance day-of-week.
    clkDow_ = static_cast<uint8_t>((clkDow_ + 1) % 7);
    if (!bcdBumpField(clkDay_, bcdDaysInMonth(clkMonth_, clkYear_), 1)) return;
    if (clkMonth_ < 12) {
        clkMonth_ = static_cast<uint8_t>(clkMonth_ + 1);
        return;
    }
    clkMonth_ = 1;
    clkYear_++;
}

uint64_t Upd1990ac::liveTimeAsBcd40() const {
    uint64_t reg = 0;
    reg |= static_cast<uint64_t>(clkSec_  & 0x0F) << 0;
    reg |= static_cast<uint64_t>((clkSec_  >> 4) & 0x0F) << 4;
    reg |= static_cast<uint64_t>(clkMin_  & 0x0F) << 8;
    reg |= static_cast<uint64_t>((clkMin_  >> 4) & 0x0F) << 12;
    reg |= static_cast<uint64_t>(clkHour_ & 0x0F) << 16;
    reg |= static_cast<uint64_t>((clkHour_ >> 4) & 0x0F) << 20;
    reg |= static_cast<uint64_t>(clkDay_  & 0x0F) << 24;
    reg |= static_cast<uint64_t>((clkDay_  >> 4) & 0x0F) << 28;
    reg |= static_cast<uint64_t>(clkDow_   & 0x0F) << 32;
    reg |= static_cast<uint64_t>(clkMonth_ & 0x0F) << 36;
    return reg;
}

void Upd1990ac::commitShiftRegisterToTime() {
    auto nibble = [this](int shift) { return static_cast<int>((shiftRegister_ >> shift) & 0x0F); };
    clkSec_   = static_cast<uint8_t>((nibble(4)  << 4) | nibble(0));
    clkMin_   = static_cast<uint8_t>((nibble(12) << 4) | nibble(8));
    clkHour_  = static_cast<uint8_t>((nibble(20) << 4) | nibble(16));
    clkDay_   = static_cast<uint8_t>((nibble(28) << 4) | nibble(24));
    clkDow_   = static_cast<uint8_t>(nibble(32));
    clkMonth_ = static_cast<uint8_t>(nibble(36));
    // clkYear_ kept as-is: the chip has no year field, so a TIME= that
    // changes the month can't move the year.
    rtcAccumSeconds_ = 0.0;
}

void Upd1990ac::seedFromHost(int year, int month, int day, int hour, int minute, int second, int dow,
                             int millisecond) {
    clkYear_  = year;
    clkMonth_ = static_cast<uint8_t>(month);
    clkDay_   = bcdPack(day);
    clkHour_  = bcdPack(hour);
    clkMin_   = bcdPack(minute);
    clkSec_   = bcdPack(second);
    clkDow_   = static_cast<uint8_t>(dow & 0x07);
    rtcAccumSeconds_ = millisecond / 1000.0;
}

bool Upd1990ac::dataOut() const {
    if (mode_ == Mode::RegisterShift || mode_ == Mode::TimeSet)
        return (shiftRegister_ & 1) != 0;
    // Register Hold / Time Read: DATA OUT is a fixed status waveform (per
    // the datasheet ~1 Hz / 0.5 Hz), not register content. Nothing in the
    // ROM samples DATA OUT outside shift mode, so the exact phase is
    // immaterial -- a slow parity of elapsed time is enough.
    long long s = static_cast<long long>(rtcElapsedSeconds_);
    if (mode_ == Mode::TimeRead) return (s % 2) != 0;      // ~0.5 Hz
    return (static_cast<long long>(rtcElapsedSeconds_ * 2.0) % 2) != 0; // ~1 Hz
}

bool Upd1990ac::tpLevelNow() const {
    // Parity of the half-periods the free-running divider has counted --
    // a pure function of elapsed cycle-time, recomputed fresh rather than
    // incrementally accumulated, so it's exact however long it's been
    // since the last sync.
    double halfPeriod = 0.5 / tpRateHz_;
    long long intervals = static_cast<long long>(rtcElapsedSeconds_ / halfPeriod);
    return (intervals % 2) != 0;
}

void Upd1990ac::syncTp() {
    if (!tpConfigured_) return;
    bool newLevel = tpLevelNow();
    if (newLevel && !tpLevel_) tpEdgePending_ = true;
    tpLevel_ = newLevel;
}

bool Upd1990ac::tp() {
    // Always a fresh, undebounced resample -- OPB is the "primary" signal
    // here; consumeRisingEdge() (IF's read), when it happens shortly
    // afterward, instead trusts what this just established rather than
    // independently re-deriving a possibly-disagreeing answer (see class
    // doc comment).
    syncTp();
    lastOpbSyncSeconds_ = elapsedSeconds_;
    everOpbSynced_ = true;
    bool edge = tpEdgePending_;
    tpEdgePending_ = false;
    return tpLevel_ || edge;
}

bool Upd1990ac::consumeRisingEdge() {
    // Debounced against tp()'s own most-recent-read timestamp -- see class
    // doc comment for why. Falls back to an independent fresh resample
    // when no recent OPB read exists at all (BREAK's own detection path
    // never reads OPB).
    if (!everOpbSynced_ || (elapsedSeconds_ - lastOpbSyncSeconds_) >= kTpResyncDebounceSeconds) {
        syncTp();
    }
    bool edge = tpEdgePending_;
    tpEdgePending_ = false;
    return edge;
}
