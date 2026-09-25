#include "PC1600SubCpu.hpp"

#include <cstring>

#include "../BcdCalendar.hpp"

#ifdef PC1600_POWER_PROBE
#include "PC1600PowerProbe.hpp" // throw-away OFF-key trace instrumentation
#endif

namespace {
// The clock's wire layout, described once for both directions: a single
// month nibble, then these four fields as BCD pairs (high nibble first).
// Actions 6CH (publish) and 6DH (set) both walk this, so the field order
// and the stack indices can't drift apart between them.
constexpr uint8_t PC1600SubCpu::DateTime::* kClockPairFields[] = {
    &PC1600SubCpu::DateTime::day,
    &PC1600SubCpu::DateTime::hour,
    &PC1600SubCpu::DateTime::minute,
    &PC1600SubCpu::DateTime::second,
};
constexpr size_t kClockPairCount = 4;

} // namespace

void PC1600SubCpu::tickOneSecond() {
    if (!bcdBumpField(m_clock.second, 59, 0)) return;
    if (!bcdBumpField(m_clock.minute, 59, 0)) return;
    if (!bcdBumpField(m_clock.hour, 23, 0)) return;
    if (!bcdBumpField(m_clock.day, bcdDaysInMonth(m_clock.month, m_year), 1)) return;
    if (m_clock.month < 12) {
        m_clock.month = static_cast<uint8_t>(m_clock.month + 1);
        return;
    }
    m_clock.month = 1;
    m_year++;
}

void PC1600SubCpu::push(uint8_t nibble) {
    if (m_sp >= kStackSize) return; // bounded; see kStackSize's own comment
    m_stack[m_sp++] = static_cast<uint8_t>(nibble & 0x0F);
}

void PC1600SubCpu::command(uint8_t cmd) {
#ifdef PC1600_POWER_PROBE
    pc1600probe::onSubCpuCommand(cmd);
#endif
    // Every command byte crosses the parallel port and starts the BUSY
    // window firmware paces the handshake against (see busy()).
    m_busyTStatesLeft = kBusyTStates;

    const uint8_t oper  = static_cast<uint8_t>(cmd >> 4);
    const uint8_t value = static_cast<uint8_t>(cmd & 0x0F);

    switch (oper) {
        // The nibble arrives one's-complemented within its nibble (0x0F -
        // value). 0x is exactly 7x preceded by a stack reset.
        case 0x0: m_sp = 0; [[fallthrough]];
        case 0x7: push(static_cast<uint8_t>(0x0F - value)); return;

        // 4FH / 4EH: the boot ROM's sub-CPU capability probe, timer IOCS
        // 25H (romVI-6 A951). It sends B0H then B1H (complemented on the
        // wire) and expects AAH then 55H. A sub-CPU that answers both gets
        // F0B8H bit 0 set, and from then on command bytes go straight out.
        // Otherwise the ROM (A9FB) holds every byte until the next 64 Hz
        // PB5 transition. That costs up to 7.8 ms per byte, and inside the
        // 0.5 s ISR (two bytes, ~16 ms) it swallows the PB5 edges the BEEP
        // repeat loop counts. A real unit's BEEP repeats never slip a tick,
        // so it takes the fast path.
        case 0x4:
            if (value == 0x0F) { m_answer = 0xAA; m_answerPending = true; }
            else if (value == 0x0E) { m_answer = 0x55; m_answerPending = true; }
            return;
        case 0x5: request(value); return;
        case 0x6: action(value);  return;

        // 9x begins an ALARM$/WAKE$ definition and Ax ends an "ON ADIN"
        // sequence. Both are accepted and dropped -- the state they would
        // set up is not modelled (see the class comment's scope note).
        case 0x9: case 0xA: return;

        // Cx/3 parks the nibble stack on the analog-input area.
        case 0xC: if (value == 0x03) m_sp = kAnalogStackBase; return;

        default: return; // unknown operation: ignored, like the real part
    }
}

void PC1600SubCpu::request(uint8_t which) {
    switch (which) {
        // 51H/53H: issued during initialisation, each followed immediately
        // by a read. The ROM only checks that *an* answer comes back; the
        // meaning of the value is not known from any source available here.
        case 0x01: case 0x03: m_answer = 0x00; break;

        // 55H: CE-1600P Ni-Cd battery voltage. The ROM judges "low battery"
        // below A8H, so C0H reads as a healthy pack.
        case 0x05: m_answer = 0xC0; break;

        // 56H: raw analog input port.
        case 0x06: m_answer = m_analog; break;

        // 57H: PC-1600 main-unit supply voltage. Low battery below AFH,
        // released again above BEH -- C0H is clear of both thresholds.
        case 0x07: m_answer = 0xC0; break;

        // 5AH: the first request the boot ROM ever issues -- the reset
        // cause. Bit 5 tells the ROM whether to run a full ALL RESET
        // (wipe clock + RAM + settings) or preserve everything. See
        // setResetCauseAllReset()/setResetCauseSimple().
        case 0x0A: m_answer = m_resetCause; break;

        // 5CH: signal states, both reported inverted -- bit5 = 0 when the
        // CI (serial carrier-in) line is high, bit2 = 0 when the PASSWORD
        // line is high. No serial peripheral is modelled, so CI reads low
        // (bit5 = 1); bit2 tracks whether a password has been stored.
        case 0x0C:
            m_answer = static_cast<uint8_t>(0x20 | (passwordSet() ? 0x04 : 0x00));
            break;

        // 5DH: the sub-CPU's interrupt-cause byte (SRIRQ). The RAM-disk /
        // file IOCS readiness handshake (romIV-6 A8F0-region) polls this
        // and gates on bit 1 = the 0.5 s timer signal, which the real
        // sub-CPU runs as a free square wave off its own crystal -- the
        // firmware waits to see it toggle (retries ~24x, then times out to
        // BASIC ERROR 163). Driven here by PC1600Machine's 0.5 s
        // accumulator via setHalfSecondSignal(); the other cause bits stay
        // 0 until something raises them.
        case 0x0D: m_answer = m_halfSecondSignal ? 0x02 : 0x00; break;
        // 5EH: read back the SC-7852 interrupt mask set via 5FH.
        case 0x0E: m_answer = m_irqMask;  break;

        // 5FH: set the SC-7852 interrupt mask from the first two stacked
        // nibbles (high nibble first).
        case 0x0F:
            m_irqMask = static_cast<uint8_t>((m_stack[0] << 4) | (m_stack[1] & 0x0F));
            break;

        // Unknown request: leave the previous answer standing rather than
        // inventing one, matching how the real part keeps driving its last
        // value (see readAnswer()'s comment).
        default: break;
    }
    m_answerPending = true;
}

void PC1600SubCpu::action(uint8_t which) {
    switch (which) {
        // 64H: clear the password -- but only if the nibbles just pushed
        // match the stored one, i.e. the caller proved it knew the password.
        case 0x04:
            if (std::memcmp(m_password.data(), m_stack.data(), kPasswordLen) == 0) {
                m_password[0] = 0;
            }
            return;

        // 65H: store a new password from the stack.
        case 0x05:
            std::memcpy(m_password.data(), m_stack.data(), kPasswordLen);
            return;

        // 67H/69H/6BH: commit ALARM$ / TIME_CHECK$ / WAKE$(0) from the
        // stack. Accepted and dropped -- not modelled (class comment).
        case 0x07: case 0x09: case 0x0B: return;

        // 6CH: "ask for time" -- publish the clock into the stack as nine
        // nibbles, then rewind so the Z-80 can read them back out.
        case 0x0C:
            m_sp = 0;
            push(m_clock.month);
            for (auto field : kClockPairFields) {
                const uint8_t v = m_clock.*field;
                push(static_cast<uint8_t>(v >> 4));
                push(static_cast<uint8_t>(v & 0x0F));
            }
            m_sp = 0;
            return;

        // 6FH: "pop" -- move the next stacked nibble into the answer
        // register so the following IN A,(33H) reads it. This is the read
        // side of 6CH: the ROM sends one 6FH per nibble to walk the nine
        // clock nibbles back out. Without it, TIME$ always reads as zeros.
        case 0x0F:
            m_answer = (m_sp < kStackSize) ? m_stack[m_sp] : 0x00;
            if (m_sp < kStackSize) m_sp++;
            m_answerPending = true;
            return;

        // 6DH: set the clock from the stack. A field whose nibbles are all
        // 1s (0FH) means "leave this one alone" -- that is how the ROM
        // implements a partial TIME$ assignment.
        case 0x0D: {
            // The boot ROM unconditionally re-inits the calendar to
            // 1 Jan 00:00:00 on cold start -- our reset() wipes internal
            // RAM, so it always reads as a dead-battery cold boot. When
            // the host has seeded a real time (see armHostSeedGuard()),
            // let exactly that one default write pass through without
            // touching the clock: on hardware the RTC keeps running on
            // standby power and this cold-init only fires with a truly
            // dead clock. Any other 6DH -- a real TIME$= -- disarms the
            // guard and is applied normally.
            if (m_hostSeedGuard) {
                m_hostSeedGuard = false;
                const bool coldDefault =
                    m_stack[0] == 0x01 && m_stack[1] == 0x00 && m_stack[2] == 0x01 &&
                    m_stack[3] == 0x00 && m_stack[4] == 0x00 && m_stack[5] == 0x00 &&
                    m_stack[6] == 0x00 && m_stack[7] == 0x00 && m_stack[8] == 0x00;
                if (coldDefault) return;
            }
            if (m_stack[0] != 0x0F) m_clock.month = m_stack[0];
            for (size_t i = 0; i < kClockPairCount; i++) {
                const size_t hi = 1 + 2 * i; // month occupies index 0
                if (m_stack[hi] == 0x0F || m_stack[hi + 1] == 0x0F) continue;
                m_clock.*kClockPairFields[i] =
                    static_cast<uint8_t>(((m_stack[hi] & 0x0F) << 4) |
                                          (m_stack[hi + 1] & 0x0F));
            }
            return;
        }

        default: return;
    }
}

uint8_t PC1600SubCpu::readAnswer() {
    // Reading the answer register ends the parallel transaction -- close
    // the BUSY window even if step()'s T-state pump hasn't run (standalone
    // SC7852 + PC1600Memory use, e.g. sc7852_tests' boot smoke).
    m_busyTStatesLeft = 0;
    m_answerPending = false;
    return m_answer;
}
