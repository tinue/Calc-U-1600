#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ExpansionCard.hpp"
#include "../Serial/SerialLink.hpp"

// ── CE-158 RS-232C / Centronics interface (60-pin bus) ──────────────────
//
// Like the CE-150, the CE-158 has no CPU: its 16 KB firmware runs on the
// host LH5801 from a ROM window this card supplies, and drives two I/O
// chips of its own. It works on the 60-pin bus alone or chained behind a
// CE-150 (plugged into the CE-150's rear connector). The two cards'
// windows never overlap, so SystemBus attach order does not matter.
// On the PC-1600 the same card serves the LH5803 (MODE 1): its ROM half of
// the LH5803's 8000-BFFF peripheral window is PVOUT = 1, the CE-150's is
// PVOUT = 0 (LH5803SharedMemory routes both).
//
//   * ME0 read, guest 0x8000-0x9FFF, PV = 1  -> CE-158 ROM, PU picks the
//     8 KB bank: PU = 0 the low half of CE-158.ROM, PU = 1 the high half
//     (Jeff Birt's Sharp_CE-158 dumps `CE-158_ROM_SPV_RPU_LOW` /
//     `_SPV_SPU_HIGH` are byte-identical to the two halves of our file).
//   * ME1 0xD000-0xD1FF -> LH5811 PIO, register select = addr & 0x0F.
//   * ME1 0xD200-0xD3FF -> CDP1854A UART (CE-158 Service Manual, UART
//     pages): A0 = RSEL. Write 0xD200 = transmitter holding register,
//     0xD201 = control register; read 0xD202 = receiver holding
//     register, 0xD203 = status register.
//   * ME1 0xDE00-0xDFFF -> interrupt-ID register (Service Manual: "Interrupt
//     will proceed address of between DE00 to DFFF"). The original ROM
//     never reads it and polls the UART. It reads 0x80 here ("no interrupt").
//
// The LH5811 and UART blocks are decoded to 512 bytes (the Service Manual
// shows the LH5811 chip select built from AD15-AD9 + ME1). The UART's
// exact decode width is not documented; it is assumed to be the same.
//
// **LH5811 pins** (TRM p.108 "CE-158 I/O port" + the ROM's use of them):
//   PA0 DTR out, PA1 RTS out; PA2 CTS, PA3 CD, PA4 DSR in -- low = asserted
//   (the ROM refuses to send/receive unless `PA & 0x3C` is 0); PA5
//   low-battery in (0 = OK); PA6/PA7 inputs at power-on, read by the ROM
//   (9F8E) as a strap that picks its UART status-bit decode, then outputs
//   (baud select). They read 1/1 here: that selects the decode that tests
//   THRE (bit 7) before a send and DA (bit 0) on receive -- the CDP1854's
//   own layout.
//   PB0-PB6 Centronics DATA2-8 out, PB7 BUSY in (1 = ready: the ROM's
//   LPRINT path fails on `BIT (PB),80` = 0).
//   PC0-PC4 baud select out, PC5 Centronics DATA1 out, PC6 STROBE out
//   (pulsed high), PC7 INIT out.
//   The Centronics data lines are inverted: the ROM writes PC5 = ~D0 and
//   PB0-6 = ~D1..~D7, so the printed byte is `~((PB & 0x7F) << 1 | PC5)`.
//
// **Baud rate.** The ROM (HB_CFG_URT_BD, 8B3D) loads PC0-4 + PA7 from an
// eight-entry table; `baudRate()` maps those codes back. Only used for
// pacing -- the byte stream to the host is not bit-timed.
//
// **Scope / simplifications.**
//   - One character time = 10 bit times at the decoded baud rate,
//     whatever the word format.
//   - RX: a byte is taken from the link only when the receiver holding
//     register is empty (DA = 0), so a slow BASIC loop never loses data to
//     an overrun. The PTY's own buffer back-pressures the sender instead.
//   - TX is not gated on the control register's TR bit or the UART CTS
//     pin (their wiring is not documented); the ROM's own PA2-PA5 check
//     already gates every send.
//   - The PE/FE/OE status bits are never set (a byte stream has no line
//     errors); a control-register write clears them anyway.
class Ce158Card final : public ExpansionCard {
public:
    static constexpr size_t   kBankSize = 0x2000;      // 8 KB per PU bank
    static constexpr size_t   kRomSize  = 2 * kBankSize; // CE-158.ROM, 16384 B
    static constexpr uint16_t kRomBase  = 0x8000;
    static constexpr uint16_t kRomEnd   = 0x9FFF;
    static constexpr uint16_t kPioBase  = 0xD000;      // LH5811, ME1
    static constexpr uint16_t kPioEnd   = 0xD1FF;
    static constexpr uint16_t kUartBase = 0xD200;      // CDP1854A, ME1
    static constexpr uint16_t kUartEnd  = 0xD3FF;
    static constexpr uint16_t kIntIdBase = 0xDE00;     // interrupt-ID register, ME1
    static constexpr uint16_t kIntIdEnd  = 0xDFFF;

    // CDP1854 status register bits.
    static constexpr uint8_t kStatusDA   = 0x01; // data available
    static constexpr uint8_t kStatusOE   = 0x02; // overrun error
    static constexpr uint8_t kStatusPE   = 0x04; // parity error
    static constexpr uint8_t kStatusFE   = 0x08; // framing error
    static constexpr uint8_t kStatusTSRE = 0x40; // transmitter shift register empty
    static constexpr uint8_t kStatusTHRE = 0x80; // transmitter holding register empty

    /// The default unit tick() counts in: PC1500Machine ticks the card
    /// with LH5801 cycles (same value as Upd1990ac::kCpuHz). The PC-1600
    /// ticks it with SC7852 T-states and sets its own rate (setClockHz).
    static constexpr double kCpuHz = 1300000.0;

    /// Rate of the units tick() is given in. Recomputes the character time.
    void setClockHz(double hz) {
        m_clockHz = hz;
        updateCharCycles();
    }

    /// `data` must be exactly `kRomSize` bytes (`CE-158.ROM`).
    bool loadRom(const uint8_t* data, size_t size) {
        if (size != kRomSize) return false;
        std::memcpy(m_rom.data(), data, kRomSize);
        m_romLoaded = true;
        return true;
    }

    /// The loaded ROM (both banks), for the GUI debug "Dump Mem" view.
    std::vector<uint8_t> debugRomImage() const {
        if (!m_romLoaded) return {};
        return std::vector<uint8_t>(m_rom.begin(), m_rom.end());
    }

    /// Attach / detach the RS-232C peer. Non-owning; `nullptr` = no peer
    /// (sent bytes are dropped, nothing is ever received). reset() keeps it.
    void setSerialLink(SerialLink* link) {
        m_link = link;
        m_lastDtr = m_lastRts = -1; // re-announce our lines to the new peer
        refreshLines();
    }

    /// Re-anchor to power-on state: LH5811 latches cleared, UART idle.
    /// Keeps the serial link and any captured-but-undrained parallel output.
    void reset() {
        m_pio.fill(0);
        m_uartControl = 0;
        m_uartStatus = kStatusTHRE | kStatusTSRE;
        m_rxData = 0;
        m_txHold = 0;
        m_txPending = false;
        m_accum = 0;
        m_lastDtr = m_lastRts = -1;
        updateCharCycles();
        refreshLines();
    }

    /// Per-emulation-step hook, in host CPU cycles: advances the UART
    /// by whole character times -- sends a pending byte, takes one byte
    /// from the peer when the receiver is empty.
    void tick(uint64_t cycles) {
        m_accum += cycles;
        if (m_accum < m_charCycles) return;
        // Bound the catch-up after a long gap (e.g. the machine was paused).
        if (m_accum > m_charCycles * 64) m_accum = m_charCycles * 64;
        while (m_accum >= m_charCycles) {
            m_accum -= m_charCycles;
            refreshLines();
            if (m_txPending) {
                if (m_link) m_link->send(m_txHold);
                m_txPending = false;
                m_uartStatus |= kStatusTHRE | kStatusTSRE;
            }
            if (!(m_uartStatus & kStatusDA) && m_link) {
                uint8_t b = 0;
                if (m_link->poll(b)) {
                    m_rxData = b;
                    m_uartStatus |= kStatusDA;
                }
            }
        }
    }

    /// Bytes the ROM has strobed out of the Centronics port since the last
    /// drain, in order.
    std::vector<uint8_t> drainParallelOutput() {
        // Copy + clear (not swap): keeps the buffer's capacity for the
        // next frame's bytes instead of regrowing it every drain.
        std::vector<uint8_t> out(m_parallelOut);
        m_parallelOut.clear();
        return out;
    }

    // ── Debug / test peek ───────────────────────────────────────────────
    uint8_t uartStatus() const { return m_uartStatus; }
    uint8_t uartControl() const { return m_uartControl; }
    /// Decoded from PC0-4 + PA7; 0 when the code is not one the ROM uses.
    int baudRate() const { return decodeBaud(); }

    bool respondsToRead(const PinState& pins, uint8_t& outValue) const override {
        const uint16_t addr = pins.address;
        if (pins.me1) {
            if (addr >= kPioBase && addr <= kPioEnd) {
                outValue = readPio(uint8_t(addr & 0x0F));
                return true;
            }
            if (addr >= kUartBase && addr <= kUartEnd) {
                outValue = (addr & 1) ? m_uartStatus : readRxData();
                return true;
            }
            if (addr >= kIntIdBase && addr <= kIntIdEnd) {
                outValue = 0x80; // bit 7 = 1: no interrupt pending
                return true;
            }
            return false;
        }
        if (pins.forWrite || !pins.pin[2] /*PV*/) return false;
        if (addr < kRomBase || addr > kRomEnd || !m_romLoaded) return false;
        const size_t bank = pins.pin[3] /*PU*/ ? kBankSize : 0;
        outValue = m_rom[bank + (addr - kRomBase)];
        return true;
    }

    bool respondsToWrite(const PinState& pins, uint8_t value) override {
        if (!pins.me1) return false; // ROM window is read-only
        const uint16_t addr = pins.address;
        if (addr >= kPioBase && addr <= kPioEnd) {
            writePio(uint8_t(addr & 0x0F), value);
            return true;
        }
        if (addr >= kUartBase && addr <= kUartEnd) {
            if (addr & 1) {
                // Control register. Clears the error flags: the ROM's RX
                // error recovery (81E6) rewrites it for exactly that.
                m_uartControl = value;
                m_uartStatus &= uint8_t(~(kStatusOE | kStatusPE | kStatusFE));
            } else {
                m_txHold = value;
                m_txPending = true;
                m_uartStatus &= uint8_t(~(kStatusTHRE | kStatusTSRE));
            }
            return true;
        }
        if (addr >= kIntIdBase && addr <= kIntIdEnd) return true; // read-only; claimed
        return false;
    }

private:
    // LH5811 register select (RS0-3 = AD0-3), same layout as the PC-1500's
    // own LH5811: 8 PC, 9 G, A MSK, B IF, C DDA, D DDB, E PA, F PB.
    static constexpr uint8_t kRegPC = 0x8, kRegDDA = 0xC, kRegDDB = 0xD, kRegPA = 0xE, kRegPB = 0xF;

    // A read sees the latch on output bits and the pin level on input bits.
    uint8_t readPio(uint8_t sel) const {
        switch (sel) {
            case kRegPA: return uint8_t((m_pio[kRegPA] & m_pio[kRegDDA]) | (portAInputs() & ~m_pio[kRegDDA]));
            case kRegPB: return uint8_t((m_pio[kRegPB] & m_pio[kRegDDB]) | (0xFF & ~m_pio[kRegDDB])); // PB7 BUSY = 1: ready
            default:     return m_pio[sel];
        }
    }

    void writePio(uint8_t sel, uint8_t value) {
        const uint8_t old = m_pio[sel];
        m_pio[sel] = value;
        switch (sel) {
            case kRegPC:
                // STROBE (PC6) rising edge latches the byte into the printer.
                if ((value & 0x40) && !(old & 0x40)) {
                    const uint8_t pb = m_pio[kRegPB];
                    if (m_parallelOut.size() >= kParallelCap) // nobody draining: keep the newest half
                        m_parallelOut.erase(m_parallelOut.begin(), m_parallelOut.begin() + kParallelCap / 2);
                    m_parallelOut.push_back(uint8_t(~(((pb & 0x7F) << 1) | ((value >> 5) & 0x01))));
                }
                updateCharCycles();
                return;
            case kRegPA:
            case kRegDDA:
                updateCharCycles();
                refreshLines();
                return;
            default:
                return;
        }
    }

    uint8_t portAInputs() const {
        uint8_t v = 0xC3;                 // PA0/PA1 (outputs), PA6/PA7 strap = 1
        if (!m_cts) v |= 0x04;            // PA2 CTS, low = asserted
        if (!m_dcd) v |= 0x08;            // PA3 CD
        if (!m_dsr) v |= 0x10;            // PA4 DSR
        // PA5 low-battery = 0: the Ni-Cd pack is fine.
        return v;
    }

    uint8_t readRxData() const {
        // Reading the receiver holding register resets DA. This is a
        // const bus read in the ExpansionCard interface, so the flag is
        // mutable.
        m_uartStatus &= uint8_t(~kStatusDA);
        return m_rxData;
    }

    // The eight (PC0-4, PA7) codes HB_CFG_URT_BD writes, from its table
    // $70 $58 $56 $4C $48 $44 $42 $41 (PC = 0x80 | b >> 1, PA7 = b & 1).
    int decodeBaud() const {
        const uint8_t pc = m_pio[kRegPC] & 0x1F;
        const bool pa7 = (m_pio[kRegPA] & 0x80) != 0;
        if (pa7) return pc == 0x00 ? 2400 : 0;
        switch (pc) {
            case 0x18: return 50;
            case 0x0C: return 100;
            case 0x0B: return 110;
            case 0x06: return 200;
            case 0x04: return 300;
            case 0x02: return 600;
            case 0x01: return 1200;
            default:   return 0;
        }
    }

    void updateCharCycles() {
        int baud = decodeBaud();
        if (baud == 0) baud = 300; // before SETCOM / unknown code: the ROM's default
        m_charCycles = static_cast<uint64_t>(m_clockHz * 10.0 / baud);
    }

    // Sample the peer's lines; forward ours when they change. PA0/PA1 are
    // taken as low = asserted, like the inputs (not confirmed on hardware;
    // no current transport carries them).
    void refreshLines() {
        if (!m_link) { m_cts = m_dsr = m_dcd = true; return; }
        SerialLink::Lines in;
        m_link->getStatus(in);
        m_cts = in.cts;
        m_dsr = in.dsr;
        m_dcd = in.dcd;
        const int dtr = (m_pio[kRegDDA] & 0x01) && !(m_pio[kRegPA] & 0x01);
        const int rts = (m_pio[kRegDDA] & 0x02) && !(m_pio[kRegPA] & 0x02);
        if (dtr != m_lastDtr || rts != m_lastRts) {
            m_lastDtr = dtr;
            m_lastRts = rts;
            m_link->setControl(dtr != 0, rts != 0);
        }
    }

    std::array<uint8_t, kRomSize> m_rom{};
    bool m_romLoaded = false;

    std::array<uint8_t, 16> m_pio{};

    // CDP1854A.
    uint8_t m_uartControl = 0;
    mutable uint8_t m_uartStatus = kStatusTHRE | kStatusTSRE;
    uint8_t m_rxData = 0;
    uint8_t m_txHold = 0;
    bool m_txPending = false;
    uint64_t m_accum = 0;
    double m_clockHz = kCpuHz;
    uint64_t m_charCycles = static_cast<uint64_t>(kCpuHz * 10.0 / 300);

    SerialLink* m_link = nullptr;
    bool m_cts = true, m_dsr = true, m_dcd = true;
    int m_lastDtr = -1, m_lastRts = -1;

    static constexpr size_t kParallelCap = 1 << 20;
    std::vector<uint8_t> m_parallelOut;
};

/// Renders CE-158 parallel-port (printer) bytes for display: CR dropped,
/// LF and printable ASCII kept, anything else shown as `<XX>` (hex).
/// Shared by the GUI printer pane and the CLIs' report.
inline std::string ce158PrintableText(const uint8_t* bytes, size_t size) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string text;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t b = bytes[i];
        if (b == '\r') continue;
        if (b == '\n' || (b >= 0x20 && b < 0x7F)) {
            text += static_cast<char>(b);
        } else {
            text += '<';
            text += kHex[b >> 4];
            text += kHex[b & 0x0F];
            text += '>';
        }
    }
    return text;
}
