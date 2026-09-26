#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "../BcdCalendar.hpp"
#include "PC1600Clocks.hpp"

// ── PC-1600 sub-CPU (LU-57813P) ──────────────────────────────────────────
//
// The PC-1600's 4-bit housekeeping processor. It owns the calendar clock,
// the wake-up and alarm timers, the power-on password, the A/D inputs
// (battery levels, analog jack) and the INT6 interrupt line. It runs on
// the always-on VGG rail. No datasheet exists and its ROM is undumped, so
// this is a high-level model of what the Z-80 ROM asks of it. The command
// set and every fact below are in SharpPC1500Reference
// `PC-1600/PC-1600-SubCpu-LU57813P.md` (cited as "SubCpu §n"), rebuilt from
// the PC-1600 Service Manual §4-3/§9-3 and the ROM's timer IOCS module
// (P2-B6 A74AH-AA40H).
//
// **Transport.** The Z-80 writes the *complement* of a command byte to the
// TC8576F's parallel-data register (port 21H). The CPC drives it inverted
// onto /DATA1-8, so the sub-CPU's R13-R00 inputs see the true byte, the
// *operand*, and the CPC's DSTB pulse reaches KI (SubCpu §6,
// PC-1600-CPC-TC8576.md §9.4). Answers are read at I/O 33H, a buffer in the
// LR38041 gate array that the sub-CPU drives from R33-R20. TC8576F
// complements the register value and calls strobe(). Every operand in this
// class is in the chip's own terms, as the ROM passes it to its send
// routine (P2-B6 A974H).
//
// **Scope.** Commands whose purpose the ROM does not reveal (SubCpu §8) are
// accepted and leave the previous answer standing, as the real part keeps
// driving its last value. The clock is plain settable state. This class
// never reads the host clock, which keeps the core deterministic for
// tests; the GUI/CLI seeds it (PC1600Machine::seedClock).
class PC1600SubCpu {
public:
    /// The KI strobe: `operand` is the byte on R13-R00. `kiDelayTStates` is
    /// how long after the Z-80's write the strobe arrives (the CPC's DSTB
    /// delay, programmed by PR2/PR7 -- TC8576F computes it). The answer is
    /// latched at once so callers that don't advance time still see it;
    /// only the BUSY/ACK handshake is timed.
    void strobe(uint8_t operand, int kiDelayTStates = 0);

    /// A read of I/O 33H. Returns the current answer register.
    ///
    /// Reading does NOT clear the register, it only clears the
    /// "an answer is waiting" flag: the real part keeps driving its last
    /// answer, so this leaves the port's previous value in place whenever
    /// nothing new is pending. The boot ROM relies
    /// on it -- it reads 33H at points where no command immediately
    /// precedes the read (P2-B6 A8F5H), and an open-bus FFH there is what
    /// this class exists to stop it seeing.
    uint8_t readAnswer();

    /// Whether a request has produced an answer not yet read by the Z-80.
    bool answerPending() const { return m_answerPending; }

    // ── Handshake timing (SubCpu §6) ─────────────────────────────────
    //
    // After KI the sub-CPU pulls Z10 (-> CPC /BUSY) low while it runs the
    // command in its own ISR, and pulses Z9 (-> CPC ACK) when it is done.
    // A command with an answer (Service Manual §4-3 "type (i)") pulses Z9
    // once the answer is on R33-R20. One without an answer ("type (ii)")
    // pulses Z9 on receipt and stays busy while it executes. The ROM waits
    // for BUSY = 0 before sending (P2-B6 A97BH) and for the CPC's XBUSY,
    // which the Z9 pulse clears, after sending (A98CH), then reads 33H.
    //
    // The response time is fitted, not documented: measured 2026-09-23 on
    // a real unit, a BASIC FOR loop (2000 iterations, BEEP-bracketed) runs
    // 45.9 ms faster with the sub-CPU interrupt masked (OUT 53,&1F). The
    // emulator made that difference only 9.6 ms at a 64 T window, so each
    // of the 0.5 s ISR's command bytes (A2H, A3H) costs ~1.66 ms on
    // hardware. That total includes the CPC's own DSTB delay, 17 tSYS =
    // 27.7 us at the ROM's PR2 = 0FH / PR7 = 02H (PC-1600-CPC-TC8576.md
    // §9.1), which TC8576F now passes in as kiDelayTStates, so the sub-CPU's
    // share is the rest.
    static constexpr int kResponseMicros = 1632;
    static constexpr int kBusyTStates = static_cast<int>(kPC1600TStateHz / 1000 * kResponseMicros / 1000); // SC-7852 T-states

    /// Z10 low: the sub-CPU has taken KI and is still running the command.
    bool busy() const {
        return m_elapsed >= m_busyFrom && m_elapsed < m_busyUntil;
    }
    /// Z9 has pulsed since the last strobe(). The CPC's XBUSY clears on it.
    bool acked() const { return m_elapsed >= m_ackAt; }
    /// An answer is waiting AND has been acknowledged -- the state in which
    /// the ROM reads port 33H.
    bool answerReady() const { return m_answerPending && acked() && !busy(); }
    /// Advance the handshake timeline by `tstates` SC-7852 T-states. Driven
    /// from PC1600Machine::step() alongside the timer/RTC accumulators.
    void tickByTStates(int tstates) {
        if (m_elapsed < m_busyUntil) {
            m_elapsed += tstates;
            if (m_elapsed > m_busyUntil) m_elapsed = m_busyUntil;
        }
    }

    // ── Injected state (host-side; never read from the host clock here) ──

    /// Clock, as the sub-CPU stores it: `month` 1-12, `day`/`hour`/`minute`/
    /// `second` as BCD pairs -- the wire layout (SubCpu §7.3).
    struct DateTime { uint8_t month, day, hour, minute, second; };

    // Pack/unpack one BCD byte (two decimal digits, high nibble first).
    // Used by tickOneSecond()'s field rollover and shared with
    // PC1600Machine::seedClock() so the encoding lives in one place.
    static uint8_t packBcd(int v) { return bcdPack(v); }
    static int unpackBcd(uint8_t v) { return bcdUnpack(v); }

    void setDateTime(const DateTime& dt) { m_clock = dt; }
    DateTime dateTime() const { return m_clock; }

    /// The year the host seeded, used only for February's length. The
    /// chip keeps no year (BASIC's DATE$ is MM/DD), and the Service Manual
    /// (§4-2) says its clock has "no leap-year handling". Which length its
    /// February has is not documented, so this keeps the calendar one
    /// until it's measured on a real unit (TODO.md). tickOneSecond() bumps
    /// it when December rolls into January.
    void setYear(int year) { m_year = year; }
    int  year() const { return m_year; }

    /// Arm the one-shot cold-start guard: the next clock write (SWRT, 92H)
    /// carrying the boot ROM's 1 Jan 00:00:00 default is swallowed instead
    /// of applied, so a host-seeded time survives boot. The host calls this
    /// right after seeding (see PC1600Machine::seedClock). Harmless if the
    /// ROM never issues that write -- the guard just stays armed until the
    /// first SWRT, cold-default or not.
    void armHostSeedGuard() { m_hostSeedGuard = true; }

    /// The reset / power-on cause answered to A5H (IOCS 15H), which the boot
    /// ROM reads first and reorders into FA1BH (SubCpu §4.1). Bit 7 = RESET
    /// switch, bit 5 = ALL RESET (the ROM then wipes the clock, the work
    /// area and every setting), bit 3 = ON key, bit 1 = wake-up timer,
    /// bit 0 = RS-232C CI. 0xA0 is what a cold power-up reports, 0x80 a
    /// plain RESET.
    void setResetCauseAllReset() { m_resetCause = 0xA0; }
    void setResetCauseSimple()   { m_resetCause = 0x80; }

    /// Advance the clock by exactly one second, carrying through
    /// minute/hour/day/month. day/hour/minute/second are stored BCD
    /// (a nibble pair per field, as the protocol ships them), so the carry
    /// is BCD arithmetic; month is plain 1-12. Driven by PC1600Machine's
    /// 1 Hz accumulator -- i.e. off emulated cycles, exactly as the real
    /// chip runs off its own crystal. Raises the 1 s interrupt bit, and at
    /// each minute carry compares the clock with the three timers
    /// (Service Manual §7-1).
    void tickOneSecond();

    /// The 0.5 s tick of the sub-CPU's divider: raises SRIRQ bit 1.
    /// PC1600Machine derives it from the same divider as the 64 Hz Z6
    /// output (kTimer64EdgesPerHalfSecond).
    void halfSecondTick() { raise(kIrqHalfSecond); }

    /// The ALL RESET switch (ACL): the sub-CPU restarts and loses its
    /// timers, masks, password and pending interrupts. The clock value is
    /// kept here -- the ROM rewrites it on ALL RESET anyway, and a
    /// host-seeded time must survive that (armHostSeedGuard()).
    void aclReset();

    // ── Timers and the interrupt line (SubCpu §5, §7.3) ─────────────────

    /// A wake-up / alarm setting as stored: month nibble (0FH = any), then
    /// BCD pairs whose all-F value means "any". Month 0 never matches, which
    /// is what SINIT writes on ALL RESET (P2-B6 A846H).
    struct Alarm { uint8_t month{0}, day{0}, hour{0}, minute{0}; };
    enum Timer { WakeUp = 0, Alarm1 = 1, Alarm2 = 2 };
    Alarm timer(Timer t) const { return m_timers[t]; }

    // SRIRQ / SWMSK bits (PC-1600-IO-Ports.md §7.1).
    static constexpr uint8_t kIrqWakeUp     = 0x80;
    static constexpr uint8_t kIrqAlarm1     = 0x40; // ON TIME$
    static constexpr uint8_t kIrqAlarm2     = 0x20; // ALARM$
    static constexpr uint8_t kIrqOneSecond  = 0x04;
    static constexpr uint8_t kIrqHalfSecond = 0x02;

    /// Events raised and not yet read by SRIRQ, whatever the mask.
    uint8_t pendingInterrupts() const { return m_pending; }
    /// Z7 -> INT6 (port 32H bit 6): a pending event the mask enables. The
    /// SRIRQ read (A2H) clears every pending bit and so drops the line.
    bool interruptRequest() const { return (m_pending & m_irqMask) != 0; }
    /// Called whenever interruptRequest() changes.
    void setInterruptHook(std::function<void()> hook) { m_intHook = std::move(hook); }

    /// The analog-input jack's A/D value, answered to SRA1 (A9H).
    void setAnalogInput(uint8_t v) { m_analog = v; }

    /// The RS-232C CI (ring) line, sampled on Q1 (Service Manual §9-3).
    /// SRINP (A3H) reports it in bit 5, inverted.
    void setCiLine(bool asserted) { m_ci = asserted; }

    uint8_t interruptMask() const { return m_irqMask; }
    bool passwordSet() const { return m_password[0] != 0; }

private:
    void raise(uint8_t bits) { m_pending |= bits; refreshInterrupt(); }
    void refreshInterrupt();
    void storeTimer(Timer t);
    void publishTimer(Timer t);
    void compareTimers();
    void carryIntoHour();

    /// Executes `operand` (SubCpu §7.2). Returns true if the command
    /// answers on R33-R20 (type (i)), which decides when Z9 pulses.
    bool execute(uint8_t operand);
    void appendParam(uint8_t nibble);
    void setAnswer(uint8_t v) { m_answer = v; m_answerPending = true; }
    /// The nibble at `i` of the parameter buffer, 0 past its end.
    uint8_t param(size_t i) const { return i < m_paramLen ? m_param[i] : 0; }

    // Parameters arrive one nibble per command: F0H+n starts a block,
    // 80H+n appends. The largest block is 16 nibbles (the 8-byte PASS
    // functions, IOCS 0AH-0EH).
    static constexpr size_t kParamMax = 16;
    // Results longer than a byte (the clock, 9 nibbles) are fetched one
    // nibble per 90H.
    static constexpr size_t kResultMax = 9;
    static constexpr size_t kPasswordLen = 16;

    std::array<uint8_t, kParamMax> m_param{};
    size_t m_paramLen{0};
    std::array<uint8_t, kResultMax> m_result{};
    size_t m_resultLen{0};
    size_t m_resultPos{0};
    // The last command that takes trailing 80H+n nibbles without an
    // execute byte of its own: 3CH (SWA1A thresholds), 6AH (SWAB). 0 = none.
    uint8_t m_prefix{0};

    std::array<uint8_t, kPasswordLen> m_password{};
    uint8_t  m_answer{0};
    bool     m_answerPending{false};
    // Handshake timeline since the last strobe(), in SC-7852 T-states.
    // The initial values put it at rest: not busy, acknowledged.
    int      m_elapsed{0};
    int      m_busyFrom{0};
    int      m_busyUntil{0};
    int      m_ackAt{0};
    uint8_t  m_irqMask{0};
    uint8_t  m_pending{0};
    bool     m_irqOut{false};
    std::function<void()> m_intHook;
    std::array<Alarm, 3> m_timers{};
    uint8_t  m_analog{0};
    uint8_t  m_adinLow{0}, m_adinHigh{0}; // SWA1A thresholds (3CH), stored only
    uint8_t  m_alarmSignal{0};            // SWAB nibble (6AH), stored only
    uint8_t  m_powerOnMask{0};            // SWPON nibble (A4H), SubCpu §7.2
    bool     m_ci{false};
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
