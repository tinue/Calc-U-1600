#include "PC1600SubCpu.hpp"

#include <cstring>

#include "../BcdCalendar.hpp"

#ifdef PC1600_POWER_PROBE
#include "PC1600PowerProbe.hpp" // throw-away OFF-key trace instrumentation
#endif

namespace {
// The clock's wire layout (SubCpu §7.3), described once for both
// directions: a single month nibble, then these four fields as BCD pairs
// (high nibble first). SWRT (92H) and SRRT (93H) both walk this, so the
// field order can't drift apart between them.
constexpr uint8_t PC1600SubCpu::DateTime::* kClockPairFields[] = {
    &PC1600SubCpu::DateTime::day,
    &PC1600SubCpu::DateTime::hour,
    &PC1600SubCpu::DateTime::minute,
    &PC1600SubCpu::DateTime::second,
};
constexpr size_t kClockPairCount = 4;

} // namespace

void PC1600SubCpu::tickOneSecond() {
    raise(kIrqOneSecond);
    if (!bcdBumpField(m_clock.second, 59, 0)) return;
    if (bcdBumpField(m_clock.minute, 59, 0)) carryIntoHour();
    // "At each minute carry it compares the time against the wake-up and
    // alarm settings" (Service Manual §7-1).
    compareTimers();
}

void PC1600SubCpu::carryIntoHour() {
    if (!bcdBumpField(m_clock.hour, 23, 0)) return;
    if (!bcdBumpField(m_clock.day, bcdDaysInMonth(m_clock.month, m_year), 1)) return;
    if (m_clock.month < 12) {
        m_clock.month = static_cast<uint8_t>(m_clock.month + 1);
        return;
    }
    m_clock.month = 1;
    m_year++;
}

namespace {
// A stored field matches when it equals the clock's, or when it is a
// wildcard: BASIC sends a '?' digit as nibble F (SubCpu §7.3). BCD digits
// are never F, so any F nibble marks the field.
bool fieldMatches(uint8_t stored, uint8_t now) {
    return (stored & 0x0F) == 0x0F || (stored >> 4) == 0x0F || stored == now;
}
} // namespace

void PC1600SubCpu::compareTimers() {
    static constexpr uint8_t kBit[3] = {kIrqWakeUp, kIrqAlarm1, kIrqAlarm2};
    for (int i = 0; i < 3; i++) {
        const Alarm& a = m_timers[i];
        if (a.month == 0) continue; // cleared (SINIT's default)
        if (a.month != 0x0F && a.month != m_clock.month) continue;
        if (!fieldMatches(a.day, m_clock.day) || !fieldMatches(a.hour, m_clock.hour) ||
            !fieldMatches(a.minute, m_clock.minute)) continue;
        raise(kBit[i]);
    }
}

void PC1600SubCpu::refreshInterrupt() {
    const bool out = interruptRequest();
    if (out == m_irqOut) return;
    m_irqOut = out;
    if (m_intHook) m_intHook();
}

void PC1600SubCpu::aclReset() {
    m_timers = {};
    m_irqMask = 0;
    m_pending = 0;
    m_password = {};
    m_powerOnMask = 0;
    m_alarmSignal = 0;
    m_adinLow = m_adinHigh = 0;
    m_paramLen = m_resultLen = m_resultPos = 0;
    m_prefix = 0;
    refreshInterrupt();
}

void PC1600SubCpu::storeTimer(Timer t) {
    // Same 9-nibble block as the clock; a timer has no seconds.
    Alarm& a = m_timers[t];
    a.month  = param(0);
    a.day    = static_cast<uint8_t>((param(1) << 4) | param(2));
    a.hour   = static_cast<uint8_t>((param(3) << 4) | param(4));
    a.minute = static_cast<uint8_t>((param(5) << 4) | param(6));
}

void PC1600SubCpu::publishTimer(Timer t) {
    // Read back as 7 nibbles: month, then day/hour/minute pairs (P2-B6
    // A881H fetches 1 + 3 x 2).
    const Alarm& a = m_timers[t];
    const uint8_t fields[3] = {a.day, a.hour, a.minute};
    m_resultLen = 0;
    m_result[m_resultLen++] = a.month;
    for (uint8_t v : fields) {
        m_result[m_resultLen++] = static_cast<uint8_t>(v >> 4);
        m_result[m_resultLen++] = static_cast<uint8_t>(v & 0x0F);
    }
    m_resultPos = 0;
}

void PC1600SubCpu::appendParam(uint8_t nibble) {
    if (m_paramLen >= kParamMax) return; // no ROM block is longer
    m_param[m_paramLen++] = static_cast<uint8_t>(nibble & 0x0F);
}

void PC1600SubCpu::strobe(uint8_t operand, int kiDelayTStates) {
#ifdef PC1600_POWER_PROBE
    pc1600probe::onSubCpuCommand(operand);
#endif
    const bool answers = execute(operand);
    // KI arrives kiDelayTStates after the Z-80's write. The sub-CPU is busy
    // from then for its response time; Z9 pulses at the end for a command
    // with an answer, on receipt otherwise (see busy()/acked()).
    m_elapsed   = 0;
    m_busyFrom  = kiDelayTStates;
    m_busyUntil = kiDelayTStates + kBusyTStates;
    m_ackAt     = answers ? m_busyUntil : kiDelayTStates;
}

bool PC1600SubCpu::execute(uint8_t op) {
    // Parameter nibbles, SubCpu §7.1.
    switch (op & 0xF0) {
        case 0xF0: // first nibble of a block
            m_paramLen = 0;
            m_prefix = 0;
            appendParam(op);
            return false;
        case 0x80: // next nibble
            appendParam(op);
            // SWAB (6AH) takes its value as one trailing nibble; SWA1A (3CH)
            // takes its two threshold bytes as four (P2-B6 A8B4H, A944H).
            if (m_prefix == 0x6A) m_alarmSignal = static_cast<uint8_t>(op & 0x0F);
            if (m_prefix == 0x3C && m_paramLen == 4) {
                m_adinLow  = static_cast<uint8_t>((param(0) << 4) | param(1));
                m_adinHigh = static_cast<uint8_t>((param(2) << 4) | param(3));
            }
            return false;
        default: break;
    }

    switch (op) {
        // Fetch the next result nibble (A9B6H). The ROM writes the raw 6FH
        // without its usual complement, so this is operand 90H.
        case 0x90:
            setAnswer(m_resultPos < m_resultLen ? m_result[m_resultPos++] : 0x00);
            return true;

        // SBEEP (IOCS 01H): key click on the F pin. The tone is unmeasured,
        // so it isn't generated (TODO.md, F-pin tones).
        case 0x91: return false;

        // SWRT (IOCS 02H): set the clock from the 9-nibble block. A field
        // whose nibbles are all F means "leave this one alone" -- that is how
        // the ROM implements a partial TIME$ / DATE$ assignment.
        case 0x92: {
            // The boot ROM unconditionally re-inits the calendar to
            // 1 Jan 00:00:00 on cold start -- our reset() wipes internal
            // RAM, so it always reads as a dead-battery cold boot. When
            // the host has seeded a real time (see armHostSeedGuard()),
            // let exactly that one default write pass through without
            // touching the clock: on hardware the RTC keeps running on
            // standby power and this cold-init only fires with a truly
            // dead clock. Any other SWRT -- a real TIME$= -- disarms the
            // guard and is applied normally.
            if (m_hostSeedGuard) {
                m_hostSeedGuard = false;
                static constexpr uint8_t kColdDefault[9] = {1, 0, 1, 0, 0, 0, 0, 0, 0};
                bool coldDefault = m_paramLen == 9;
                for (size_t i = 0; i < 9 && coldDefault; i++) coldDefault = param(i) == kColdDefault[i];
                if (coldDefault) return false;
            }
            if (param(0) != 0x0F) m_clock.month = param(0);
            for (size_t i = 0; i < kClockPairCount; i++) {
                const size_t hi = 1 + 2 * i; // month occupies index 0
                if (param(hi) == 0x0F || param(hi + 1) == 0x0F) continue;
                m_clock.*kClockPairFields[i] =
                    static_cast<uint8_t>((param(hi) << 4) | param(hi + 1));
            }
            return false;
        }

        // SRRT (IOCS 03H): publish the clock as nine nibbles for 90H to fetch.
        case 0x93:
            m_resultLen = 0;
            m_result[m_resultLen++] = m_clock.month;
            for (auto field : kClockPairFields) {
                const uint8_t v = m_clock.*field;
                m_result[m_resultLen++] = static_cast<uint8_t>(v >> 4);
                m_result[m_resultLen++] = static_cast<uint8_t>(v & 0x0F);
            }
            m_resultPos = 0;
            return false;

        // SWWT / SWA1T / SWA2T (IOCS 04H/06H/08H: WAKE$(0), ON TIME$,
        // ALARM$) and the matching reads SRWT / SRA1T / SRA2T.
        case 0x94: storeTimer(WakeUp); return false;
        case 0x95: publishTimer(WakeUp); return false;
        case 0x96: storeTimer(Alarm1); return false;
        case 0x97: publishTimer(Alarm1); return false;
        case 0x98: storeTimer(Alarm2); return false;
        case 0x99: publishTimer(Alarm2); return false;

        // IOCS 0AH: store the PASS password (8 bytes = 16 nibbles).
        case 0x9A:
            for (size_t i = 0; i < kPasswordLen; i++) m_password[i] = param(i);
            return false;
        // IOCS 0BH: clear it -- but only if the nibbles just sent match the
        // stored ones, i.e. the caller proved it knew the password. The ROM
        // then checks SRINP bit 2 (A905H).
        case 0x9B: {
            bool match = true;
            for (size_t i = 0; i < kPasswordLen && match; i++) match = m_password[i] == param(i);
            if (match) m_password[0] = 0;
            return false;
        }

        // SWMSK (IOCS 10H): the interrupt mask, high nibble first.
        case 0xA0:
            m_irqMask = static_cast<uint8_t>((param(0) << 4) | param(1));
            refreshInterrupt();
            return false;
        // SRMSK (IOCS 11H).
        case 0xA1: setAnswer(m_irqMask); return true;
        // SRIRQ (IOCS 12H): the pending events, cleared by the read, which
        // drops Z7 (Service Manual §4-3). The ROM's INT6 handler keeps the
        // masked-off bits in F07EH itself (P1-B3 41A4H), SubCpu §5.
        case 0xA2:
            setAnswer(m_pending);
            m_pending = 0;
            refreshInterrupt();
            return true;
        // SRINP (IOCS 13H): bit 5 = 0 while CI is asserted (P2-B6 A396H
        // inverts it into INSTAT), bit 2 = a password is set (A905H).
        case 0xA3:
            setAnswer(static_cast<uint8_t>((m_ci ? 0x00 : 0x20) | (passwordSet() ? 0x04 : 0x00)));
            return true;
        // SWPON (IOCS 14H): the power-on condition nibble (SubCpu §7.2).
        case 0xA4: m_powerOnMask = param(0); return false;
        // IOCS 15H: the reset / power-on cause, SubCpu §4.1.
        case 0xA5: setAnswer(m_resetCause); return true;

        // SRA0 (IOCS 18H): PC-1600 main supply. The 0.5 s ISR (P1-B3 41DAH)
        // sets low battery below AFH and clears it at BEH and above; C0H is
        // clear of both.
        case 0xA8: setAnswer(0xC0); return true;
        // SRA1 (IOCS 19H): the analog-input jack.
        case 0xA9: setAnswer(m_analog); return true;
        // SRA2 (IOCS 1AH): the second supply input, KC2 = the CE-1600P pack
        // (Service Manual §9-3). The ROM judges it low below A8H.
        case 0xAA: setAnswer(0xC0); return true;
        // IOCS 1CH, read once at init; the ROM only needs an answer back.
        case 0xAC: setAnswer(0x00); return true;
        // IOCS 1EH's execute byte: analog-input interrupt mode, stored nowhere.
        case 0xAE: setAnswer(0x00); return false;

        // IOCS 25H: the boot ROM's capability probe (P2-B6 A951H). It sends
        // B0H then B1H and expects AAH then 55H. A sub-CPU that answers both
        // gets F0B8H bit 0 set, and from then on command bytes go straight
        // out. Otherwise the ROM (A9FBH) holds every byte until the next
        // 64 Hz PB5 transition. That costs up to 7.8 ms per byte, and inside
        // the 0.5 s ISR (two bytes, ~16 ms) it swallows the PB5 edges the
        // BEEP repeat loop counts. A real unit's BEEP repeats never slip a
        // tick, so it takes the fast path (docs/Decisions.md).
        case 0xB0: setAnswer(0xAA); return true;
        case 0xB1: setAnswer(0x55); return true;

        // SRPON (IOCS 21H): the SWPON nibble, fetched with one 90H.
        case 0x69:
            m_result[0] = m_powerOnMask;
            m_resultLen = 1;
            m_resultPos = 0;
            return false;
        // SWAB / SRAB (IOCS 22H/23H): followed by 80H+n to write the
        // alarm-signal nibble, or by a 90H fetch to read it.
        case 0x6A:
            m_prefix = 0x6A;
            m_result[0] = m_alarmSignal;
            m_resultLen = 1;
            m_resultPos = 0;
            return false;
        // SWA1A (IOCS 24H): the analog-input thresholds follow as four
        // 80H+n nibbles.
        case 0x3C:
            m_prefix = 0x3C;
            m_paramLen = 0;
            return false;

        // Reads whose meaning no source gives (IOCS 16H/17H/1BH/1DH/1FH,
        // SubCpu §8): the ROM reads 33H after them, which finds the previous
        // answer still standing.
        case 0xA6: case 0xA7: case 0xAB: case 0xAD: case 0xAF:
            m_answerPending = true;
            return true;

        // Everything else: 53H (after IOCS 1EH), 9CH-9FH (IOCS 0CH-0FH), E5H
        // (IOCS 26H), EAH (system off, IOCS 20H), the LH-5803's DCH. Accepted,
        // no effect (SubCpu §8).
        default: return false;
    }
}

uint8_t PC1600SubCpu::readAnswer() {
    // Reading the answer register ends the parallel transaction -- close
    // the handshake window even if step()'s T-state pump hasn't run
    // (standalone SC7852 + PC1600Memory use, e.g. sc7852_tests' boot
    // smoke). The ROM only reads 33H once Z9 has pulsed, so on a timed
    // machine this changes nothing.
    m_elapsed = m_busyUntil;
    m_answerPending = false;
    return m_answer;
}
