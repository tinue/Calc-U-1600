#pragma once
#include <algorithm>
#include <cstdint>

// ── LH5811 / LH5810 serial block: divider, G/F registers, transmitter ────
//
// The part of the LH5811 I/O chip the cassette uses (PC-1500 TRM §3-3/3-4,
// p.68-74; PC-1500 Service Manual, LH5811 pin table):
//
//   - One binary divider, clocked by the basic clock phi (φOS), reset by a
//     write to register 4. Every clock the block makes is a tap of it, so a
//     ÷n output is bit log2(n)-1 of the count: low for n/2 phi after a
//     reset, then a square wave.
//   - G (register 9): G0-2 the serial clock, phi ÷1, 2, 128, 256, 512,
//     1024, 2048 or 4096; G3 = 0 internal clock; G4 = 1 drives it out on
//     CL0. On the PC-1500, CL0 (pin 50) is wired to CL1 (pin 47), and a
//     read of MSK returns CL1 in bit 7 -- the CE-150's tape reader uses it
//     as a timer it restarts with the divider reset.
//   - F (register 7): F6 = 1 puts the cassette modulation on SDO:
//     SDO = SXO·FX + /SXO·FY. F0-2 pick FX and F3-5 FY, each phi ÷64, 128,
//     256, 512 or 1024 (codes 5-7 aren't in the table; taken as ÷1024).
//     F6 = 0: SDO is the plain serial data SXO.
//   - L (register 6): a write sends the byte as start bit, 8 data bits LSB
//     first, two stop bits, one bit per serial clock period, and clears TD
//     (IF bit 3). SXO idles at mark (1).
//
// Timing, from a real PC-1500 (A01) + CE-150 CSAVE (2026-10-04): TD is set
// when the second stop bit is done ("upon completion of serial data
// transmission"), and a write to an idle transmitter restarts the divider
// and starts a full start bit at once, so the bits and the FX/FY tones
// start in phase -- one serial-clock period per bit, whole tone cycles in
// each. So frames the CE-150 sends back to back (it waits for TD, then
// writes L) follow each other with exactly two stop bits -- the "start +
// 4 data + 6 stop" nibble frames real tapes carry -- a delay the ROM
// starts after the last TD (TIME_DELAY) is all idle mark on the tape, and
// the frame after it still starts with a whole, clean start bit, which
// the CE-150's reader needs (a start bit cut short gives ERROR 44, and
// tones out of phase with the bits give wav2bin glitches the real tape
// doesn't have). A byte written while a frame is still going waits and
// follows it (not seen from the ROMs). Not documented: what TD reads at power-on (taken as 0 --
// the CE-150 sends its first byte without waiting for TD), and the
// receive side (SD1, U, RD), which nothing here uses.
//
// Time comes in CPU cycles; `phiHz` / `cpuHz` converts (the PC-1500's φOS
// is the CPU clock, 1:1).
class LH5811Serial {
public:
    LH5811Serial(uint64_t cpuHz, uint64_t phiHz) : m_cpuHz(cpuHz), m_phiHz(phiHz) {}

    void reset() {
        m_g = m_f = 0;
        m_count = 0;
        m_phiAccum = 0;
        m_holding = false;
        m_shiftBits = 0;
        m_sxo = true;
        m_td = false;
    }

    // ── Registers ────────────────────────────────────────────────────────
    void resetDivider() { m_count = 0; }       // register 4 write
    void writeG(uint8_t v) { m_g = v; }        // register 9
    uint8_t g() const { return m_g; }
    void writeF(uint8_t v) { m_f = v; }        // register 7
    uint8_t f() const { return m_f; }
    void writeL(uint8_t v) {                   // register 6
        m_td = false;
        if (m_shiftBits == 0) {
            startFrame(v);
        } else {
            m_l = v;
            m_holding = true;
        }
    }
    bool td() const { return m_td; }

    // ── Pins ─────────────────────────────────────────────────────────────
    /// The serial clock on CL0: driven only with G4 = 1 and the internal
    /// clock (G3 = 0); low otherwise.
    bool cl0() const { return (m_g & 0x18) == 0x10 && serialClock(); }
    /// The cassette / serial output.
    bool sdo() const {
        if (!(m_f & 0x40)) return m_sxo;
        return tap(modulationDivider(m_sxo ? (m_f & 0x07) : ((m_f >> 3) & 0x07)));
    }

    /// Credits `cycles` CPU cycles. `onSegment(cycles, sdo)` is called for
    /// each stretch of them in order, with SDO's level during it, so a
    /// recorder or buzzer sees every SDO edge at its exact time.
    template <class OnSegment>
    void advance(uint32_t cycles, OnSegment&& onSegment) {
        uint32_t pending = 0; // CPU cycles at the current SDO level not yet reported
        bool level = sdo();
        while (cycles > 0) {
            // One phi tick at a time would be exact but slow; jump to the
            // next tick that can change anything (any tap we use toggles
            // on a multiple of 32 phi: ÷64 is the fastest modulation tap,
            // and the serial clock ÷1/÷2 isn't used for tape).
            // The transmitter's bit ends are timed from the write, so they
            // are a boundary too.
            uint64_t toBoundary = 32 - (m_count & 31);
            if (m_shiftBits > 0) toBoundary = std::min<uint64_t>(toBoundary, bitPeriod() - m_txPhase);
            // CPU cycles until that many phi ticks have elapsed.
            const uint64_t need = toBoundary * m_cpuHz - m_phiAccum;
            const uint64_t cyclesToBoundary = (need + m_phiHz - 1) / m_phiHz;
            if (cyclesToBoundary > cycles) {
                m_phiAccum += uint64_t(cycles) * m_phiHz;
                const uint64_t ticks = m_phiAccum / m_cpuHz;
                m_phiAccum -= ticks * m_cpuHz;
                m_count += ticks;
                if (m_shiftBits > 0) m_txPhase += ticks;
                pending += cycles;
                break;
            }
            m_phiAccum += cyclesToBoundary * m_phiHz;
            const uint64_t ticks = m_phiAccum / m_cpuHz;
            m_phiAccum -= ticks * m_cpuHz;
            m_count += ticks;
            if (m_shiftBits > 0 && (m_txPhase += ticks) >= bitPeriod()) endBit();
            pending += static_cast<uint32_t>(cyclesToBoundary);
            cycles -= static_cast<uint32_t>(cyclesToBoundary);
            const bool now = sdo();
            if (now != level) {
                onSegment(pending, level);
                pending = 0;
                level = now;
            }
        }
        if (pending) onSegment(pending, level);
    }

private:
    uint64_t m_cpuHz, m_phiHz;
    uint64_t m_phiAccum = 0; // phi-ticks * cpuHz not yet whole
    uint64_t m_count = 0;    // the divider: phi ticks since its last reset
    uint8_t m_g = 0, m_f = 0, m_l = 0;
    bool m_holding = false;  // L holds a byte the shift register hasn't taken
    uint16_t m_shift = 0;    // the frame being sent, next bit in bit 0
    uint64_t m_txPhase = 0;  // phi ticks into the current bit
    int m_shiftBits = 0;     // bits of it still to send
    bool m_sxo = true;
    bool m_td = false;

    // Bit k-1 of the count is the ÷2^k output.
    bool tap(int log2Divider) const { return log2Divider <= 0 || ((m_count >> (log2Divider - 1)) & 1) != 0; }

    static int modulationDivider(int code) { return code >= 4 ? 10 : 6 + code; } // ÷64..÷1024
    bool serialClock() const {
        static constexpr int kLog2[8] = {0, 1, 7, 8, 9, 10, 11, 12}; // ÷1, 2, 128 .. 4096
        return tap(kLog2[m_g & 0x07]);
    }

    void startFrame(uint8_t v) {
        m_shift = static_cast<uint16_t>((uint16_t(v) << 1) | 0x600); // start 0, data, 2 stop
        m_shiftBits = 11;
        m_txPhase = 0;
        m_count = 0;   // the divider restarts with the frame
        m_sxo = false; // the start bit, from now
    }

    // One serial-clock period in phi ticks: a bit's length.
    uint64_t bitPeriod() const {
        static constexpr int kLog2[8] = {0, 1, 7, 8, 9, 10, 11, 12};
        return uint64_t{1} << kLog2[m_g & 0x07];
    }

    // The current bit has lasted a serial-clock period.
    void endBit() {
        m_txPhase = 0;
        m_shift >>= 1;
        if (--m_shiftBits == 0) {
            if (m_holding) {
                m_holding = false;
                startFrame(m_l);
                return;
            }
            m_td = true;
        }
        m_sxo = m_shiftBits > 0 ? (m_shift & 1) != 0 : true;
    }
};
