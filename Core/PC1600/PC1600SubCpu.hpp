#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "../BcdCalendar.hpp"
#include "PC1600Clocks.hpp"

// ── PC-1600 sub-CPU (LU-57813P) command/answer interface ─────────────────
//
// The PC-1600's second processor. It owns the real-time clock, the
// ALARM$/WAKE$/TIME_CHECK$ registers, the power-on password, the analog
// (battery-voltage) inputs and the SC-7852 interrupt cause/mask -- i.e. the
// housekeeping the main Z-80 can't do while it is switched off.
//
// **How the Z-80 talks to it.** Not over a dedicated bus: the main CPU
// writes a one-byte *command* to I/O port 21H and reads the sub-CPU's
// one-byte *answer* back from port 33H (`IOR P` in the TRM's Z-80/LH-5803
// I/O-address table). 21H is nominally the TC8576F UART's parallel-data-out
// register (PVOUT) -- the command physically travels out through the UART's
// parallel port, which is why "the serial peripheral isn't modelled" and
// "the sub-CPU isn't modelled" are the same gap on this machine, and why
// this class lives behind the UART's port number rather than one of its
// own.
//
// **Command encoding.** The high nibble selects an operation, the low
// nibble is its argument:
//
//   0x  reset the nibble stack, then push (0x0F - arg)
//   5x  *request*: compute an answer into the answer register; the Z-80
//       reads it from port 33H on the next `IN A,(33H)`
//   6x  *action*: do something using the nibble stack (set the clock, read
//       the clock back into the stack, set/clear the password, ...)
//   7x  push (0x0F - arg) onto the nibble stack
//   9x  begin an ALARM$/WAKE$ definition -- inert here
//   Ax  end an "ON ADIN" sequence -- inert here
//   Cx  arg 3: point the stack at the analog-input area (offset 20H)
//
// Multi-byte payloads (a date, a password) are shipped one nibble per
// command byte through that stack, then committed with a single 6x action.
//
// **Scope.** Every command the boot ROM is known to issue is answered with
// a value that makes the ROM's own validity checks pass. The stateful
// features behind those answers are deliberately inert: the password is
// stored and compared but never gates anything, and ALARM$/WAKE$/
// TIME_CHECK$ are accepted and discarded. The clock is plain settable
// state -- this class never reads the host clock, keeping the core
// deterministic for tests; the GUI layer is where a real "now" would be
// injected.
class PC1600SubCpu {
public:
    /// A write to I/O port 21H.
    void command(uint8_t cmd);

    /// A read of I/O port 33H. Returns the current answer register.
    ///
    /// Reading does NOT clear the register, it only clears the
    /// "an answer is waiting" flag: the real part keeps driving its last
    /// answer, so this leaves the port's previous value in place whenever
    /// nothing new is pending. The boot ROM relies
    /// on it -- it reads 33H at points where no 21H command immediately
    /// precedes the read (bank 6, A8F5H), and an open-bus FFH there is what
    /// this class exists to stop it seeing.
    uint8_t readAnswer();

    /// Whether a request has produced an answer not yet read by the Z-80.
    bool answerPending() const { return m_answerPending; }

    // ── Parallel-port handshake timing ─────────────────────────────────
    //
    // On hardware the command byte reaches the LU-57813P through the
    // TC8576F's parallel port, and the sub-CPU pulses its BUSY line
    // (Ready -> Busy -> Ready) while it runs the command in an ISR;
    // firmware polls a UART status bit between OUT (21H) and IN (33H) to
    // pace this (romIV-6 A8F0/A974; the LH-5803 OFF loop rom1500 E538).
    // The answer register is still filled synchronously in command() so
    // every existing caller keeps working -- only busy() is time-gated.
    //
    // The window is the sub-CPU's *response time*, not just the TRM
    // figure's 13-26 us strobe/ACK pulses: the LU-57813P (4-bit, 307 kHz)
    // runs every command in its own ISR. Measured 2026-09-23 on a real
    // unit: a BASIC FOR loop (2000 iterations, BEEP-bracketed) runs 45.9 ms
    // faster with the sub-CPU interrupt masked (OUT 53,&1F). The emulator
    // made that difference only 9.6 ms at a 64 T window, so each of the
    // 0.5 s ISR's command bytes costs ~1.66 ms on hardware. P2-B6 A974 waits
    // for PSR bit 5 before sending; A98C waits for PSR bit 6 after sending,
    // then reads 33H. The TC8576F reports both while busy().
    static constexpr int kResponseMicros = 1660;
    static constexpr int kBusyTStates = static_cast<int>(kPC1600TStateHz / 1000 * kResponseMicros / 1000); // SC-7852 T-states

    /// True while the modelled BUSY window after a command is still open.
    bool busy() const { return m_busyTStatesLeft > 0; }
    /// An answer is waiting AND the BUSY window has elapsed -- the state
    /// the TC8576F reports as "answer may be read from port 33H".
    bool answerReady() const { return m_answerPending && m_busyTStatesLeft == 0; }
    /// Advance the BUSY window by `tstates` SC-7852 T-states. Driven from
    /// PC1600Machine::step() alongside the timer/RTC accumulators.
    void tickByTStates(int tstates) {
        if (m_busyTStatesLeft > 0) {
            m_busyTStatesLeft -= tstates;
            if (m_busyTStatesLeft < 0) m_busyTStatesLeft = 0;
        }
    }

    // ── Injected state (host-side; never read from the host clock here) ──

    /// Clock, as the sub-CPU stores it: `month` 1-12, `day`/`hour`/`minute`/
    /// `second` as plain binary (the nibble packing happens at the protocol
    /// boundary, not here).
    struct DateTime { uint8_t month, day, hour, minute, second; };

    // Pack/unpack one BCD byte (two decimal digits, high nibble first).
    // Used by tickOneSecond()'s field rollover and shared with
    // PC1600Machine::seedClock() so the encoding lives in one place.
    static uint8_t packBcd(int v) { return bcdPack(v); }
    static int unpackBcd(uint8_t v) { return bcdUnpack(v); }

    void setDateTime(const DateTime& dt) { m_clock = dt; }
    DateTime dateTime() const { return m_clock; }

    /// Full calendar year behind the day/month rollover (it only changes
    /// how long February is). Not part of the 9-nibble wire protocol --
    /// the real LU-57813P keeps it internally and only BASIC's DATE$
    /// surfaces it. The host seeds it once at startup; tickOneSecond()
    /// bumps it when December rolls into January.
    void setYear(int year) { m_year = year; }
    int  year() const { return m_year; }

    /// Arm the one-shot cold-start guard: the next 6DH action carrying the
    /// boot ROM's 1 Jan 00:00:00 default is swallowed instead of applied,
    /// so a host-seeded time survives boot. The host calls this right
    /// after seeding (see PC1600Machine::seedClock). Harmless if the ROM
    /// never issues that write -- the guard just stays armed until the
    /// first 6DH, cold-default or not.
    void armHostSeedGuard() { m_hostSeedGuard = true; }

    /// The reset cause the sub-CPU reports to the boot ROM's first-ever
    /// query (request 5AH). **Bit 5 set = ALL RESET**: the ROM then wipes
    /// the calendar clock, zeroes the internal-RAM work area and restores
    /// every setting to default. **Bit 5 clear = simple reset / power-on**:
    /// the ROM preserves all of it. (The other bits distinguish internal
    /// vs external reset vs power-on vs wakeup; only the two levels matter
    /// to us.) Traced through romI-0's `C=15H` IOCS -> the FA1BH bit
    /// shuffle -> SINIT at romIV-6 0xA7BE. 0xA0 is the value a real cold
    /// power-up reports; 0x80 is a plain internal RESET.
    void setResetCauseAllReset() { m_resetCause = 0xA0; }
    void setResetCauseSimple()   { m_resetCause = 0x80; }

    /// Advance the clock by exactly one second, carrying through
    /// minute/hour/day/month/year. day/hour/minute/second are stored BCD
    /// (a nibble pair per field, as the protocol ships them), so the carry
    /// is BCD arithmetic; month is plain 1-12. Driven by PC1600Machine's
    /// 1 Hz accumulator -- i.e. off emulated cycles, exactly as the real
    /// chip runs off its own crystal.
    void tickOneSecond();

    /// Analog input port reading, answered to request 06H.
    void setAnalogInput(uint8_t v) { m_analog = v; }

    uint8_t interruptMask() const { return m_irqMask; }
    bool passwordSet() const { return m_password[0] != 0; }

    /// The sub-CPU's 0.5 s timer signal, as seen in bit 1 of request 5DH.
    /// On hardware it free-runs off the sub-CPU crystal; here
    /// PC1600Machine's 0.5 s accumulator toggles it. The file/RAM-disk
    /// IOCS readiness handshake polls 5DH and waits for this to change
    /// state, so it must actually toggle, not sit at a constant.
    void toggleHalfSecondSignal() { m_halfSecondSignal = !m_halfSecondSignal; }
    bool halfSecondSignal() const { return m_halfSecondSignal; }

private:
    void push(uint8_t nibble);
    void request(uint8_t which);
    void action(uint8_t which);

    // 0x20 ("ADIN_ADR") is a valid stack pointer -- command Cx/3 parks the
    // stack there -- so the array has to be bigger than that, not exactly
    // that. Sized generously here, with every write bounded.
    static constexpr size_t kStackSize = 0x40;
    static constexpr size_t kAnalogStackBase = 0x20;
    static constexpr size_t kPasswordLen = 0x10;

    std::array<uint8_t, kStackSize> m_stack{};
    std::array<uint8_t, kPasswordLen> m_password{};
    size_t   m_sp{0};
    uint8_t  m_answer{0};
    bool     m_answerPending{false};
    int      m_busyTStatesLeft{0}; // see busy()/tickByTStates()
    bool     m_halfSecondSignal{false}; // 5DH bit 1; see toggleHalfSecondSignal()
    uint8_t  m_irqMask{0};
    uint8_t  m_analog{0};
    // 1 Jan, 00:00:00 -- an arbitrary but valid default, so a ROM that
    // reads the clock before anything sets it gets a well-formed answer
    // rather than an all-zero (month 0) one it would reject.
    DateTime m_clock{1, 1, 0, 0, 0};
    // Arbitrary non-leap default for the unseeded, deterministic core; the
    // GUI/CLI replaces it with the host year at startup (see setYear()).
    int m_year{2001};
    bool m_hostSeedGuard{false}; // see armHostSeedGuard()
    // A freshly-constructed machine has no state to keep, so it starts as
    // a cold power-up (ALL RESET); PC1600Machine::reset() switches this to
    // the simple-reset value.
    uint8_t m_resetCause{0xA0}; // see setResetCauseAllReset()/setResetCauseSimple()
};
