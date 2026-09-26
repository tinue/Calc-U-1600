#include "TC8576F.hpp"

#include "PC1600Machine.hpp"

// Bit order of the two status registers (CPC §4), kept in one place so
// ssr()/psr() and any future writer agree.
namespace {
constexpr uint8_t kSsrTxRDY = 0x01;
constexpr uint8_t kSsrRxRDY = 0x02;
constexpr uint8_t kSsrTxEMP = 0x04;
constexpr uint8_t kSsrPE    = 0x08;
constexpr uint8_t kSsrOE    = 0x10;
constexpr uint8_t kSsrFE    = 0x20;
constexpr uint8_t kSsrRBRK  = 0x40;

constexpr uint8_t kPsrFAULT = 0x01;
constexpr uint8_t kPsrSLCT  = 0x02;
constexpr uint8_t kPsrPE    = 0x04;
constexpr uint8_t kPsrP5V   = 0x08;
constexpr uint8_t kPsrPRIM  = 0x10;
constexpr uint8_t kPsrBUSY  = 0x20;
constexpr uint8_t kPsrXBUSY = 0x40;
constexpr uint8_t kPsrIntF  = 0x80;

// XCLK is CL2, 1.2288 MHz, passed through the gate array (CPC §5.3).
// Character and strobe times are scaled into SC-7852 T-states, the domain
// PC1600Machine::step() feeds tick() in.
constexpr uint32_t kXclkHz   = kPC1600Cl2Hz;
constexpr uint32_t kTStateHz = PC1600Machine::kTStateHz;
} // namespace

void TC8576F::resetChip() {
    // CPC §7: the parameter registers, the prescaler and the baud
    // generator are not reset.
    m_txEnable = m_dtr = m_rxEnable = m_sendBreak = m_rts = false;
    m_intMask1 = m_intMask2 = true;
    m_prim = false;
    m_xbusy = m_intp0 = m_intp1 = false;
    m_lastSubBusy = m_sub.busy();

    m_rxBreak = m_framingError = m_overrunError = m_parityError = false;
    m_rxReady = false;
    m_rxData = 0xFF;
    m_txData = 0;
    m_parallelIn = 0xFF;
    m_parallelOut = 0;

    // TXD is forced high and any character in progress is aborted. The
    // peer attachment (m_link) is host-owned and survives a chip reset.
    m_txFifo.clear();
    m_serialAccum = 0;
    m_txEmpty = true;
    m_txReady = true;
    if (m_link) m_link->setControl(false, false);
}

void TC8576F::resetSerialState() {
    m_txFifo.clear();
    m_serialAccum = 0;
    m_txEmpty = true;
    m_txReady = true;
    m_cts = m_dcd = m_dsr = m_ci = false;
    m_sub.setCiLine(false);
}

void TC8576F::setSerialLink(SerialLink* link) {
    m_link = link;
    if (link) return;
    // tick() does nothing without a peer, so a non-empty FIFO would hold
    // TxRDY low for good and hang a transmit poll.
    resetSerialState();
}

int TC8576F::prescaler() const {
    const int k = m_pr[7] & 0x0F;
    return k == 0 ? 16 : k; // K = 1 passes XCLK through
}

int TC8576F::dstbDelayTStates() const {
    // Td = tSYS * (PR2 + 2 + x), x = 0..1 for synchronisation to SYS_CLK
    // (CPC §8.1). x = 0 here: 17 tSYS = 27.7 us at the ROM's settings.
    const long long sysTicks = static_cast<long long>((m_pr[2] & 0x1F) + 2) * prescaler();
    return static_cast<int>(sysTicks * kTStateHz / kXclkHz);
}

uint8_t TC8576F::ssr() const {
    uint8_t v = 0;
    // TxRDY: with TxINTM = 1 (what the ROM programs) plain "buffer empty";
    // with TxINTM = 0 also needs /CTS = 0 and TxEN, the transmit-interrupt
    // condition (CPC §6.3). /CTS is tied to GND, see tick().
    const bool txRdy = m_txIntMask ? m_txReady : (m_txReady && m_txEnable);
    if (txRdy)          v |= kSsrTxRDY;
    if (m_rxReady)      v |= kSsrRxRDY;
    if (m_txEmpty)      v |= kSsrTxEMP;
    if (m_parityError)  v |= kSsrPE;
    if (m_overrunError) v |= kSsrOE;
    if (m_framingError) v |= kSsrFE;
    if (m_rxBreak)      v |= kSsrRBRK;
    // DSR (b7) is the inverted /DSR pin, which the PC-1600 wires to RXD
    // (Service Manual §9-5, printed p. 33). RXD idles at mark between
    // characters and this model has no bit-level line, so it reads 0. The
    // ROM never reads it (CPC §9.5); the peer's DSR goes to PSR PE instead.
    return v;
}

uint8_t TC8576F::psr() const {
    uint8_t v = 0;
    // The status inputs carry the RS-232C lines (CPC §9.5). The RS-232C
    // receivers pull a pin low when its line is on. FAULT is read as the pin
    // level, /SLCT and /PE inverted, so CS reads 0 when on and CD/DR read 1.
    // The ROM flips bit 0 before comparing (P2-B6 A526H XOR 01H).
    if (!m_cts) v |= kPsrFAULT;
    if (m_dcd)  v |= kPsrSLCT;
    if (m_dsr)  v |= kPsrPE;
    v |= kPsrP5V; // /P5V is tied to GND (TRM §7.6), read inverted
    if (m_prim) v |= kPsrPRIM;
    // BUSY: the inverted /BUSY pin, the sub-CPU's Z10 (SubCpu §6). The ROM
    // waits for it to clear before each command byte (P2-B6 A97BH).
    if (m_sub.busy()) v |= kPsrBUSY;
    // XBUSY: set by the 21H write, cleared by the sub-CPU's Z9 ACK. The ROM
    // waits for it after each command byte (A98CH).
    if (m_xbusy && !m_sub.acked()) v |= kPsrXBUSY;
    if (intF()) v |= kPsrIntF;
    return v;
}

bool TC8576F::intF() const {
    // Factor 1 (handshake) with PR6 routing it; factor 2 (status-line
    // edges) is not modelled -- the PC-1600 keeps IM2 set.
    return !m_intMask1 && (((m_pr[6] & 0x01) && m_intp0) || ((m_pr[6] & 0x02) && m_intp1));
}

uint8_t TC8576F::readRegister(uint8_t reg) {
    switch (reg & 0x03) {
        case 0x00: // 20H -- serial input data
            m_rxReady = false;
            return m_rxData;
        case 0x01: // 21H -- parallel input data; output mode, nothing latched
            return m_parallelIn;
        case 0x02: // 22H -- serial status
            return ssr();
        case 0x03: // 23H -- parallel status
        default:
            return psr();
    }
}

void TC8576F::writeRegister(uint8_t reg, uint8_t value) {
    if ((reg & 0x03) == 0x03) { writeControlRegister(value); return; }
    if (m_resetHeld) return; // held in reset: only 23H gets through
    switch (reg & 0x03) {
        case 0x00: // 20H -- serial output data
            m_txData = value;
            if (!m_link) {
                // No peer: the byte is "sent" at once. Keep TxRDY/TxEMP set
                // so a polling transmit loop makes progress.
                m_txReady = true;
                m_txEmpty = true;
                return;
            }
            // Peer attached: queue the byte for tick() to shift out at the
            // baud rate. TxRDY drops only when the FIFO is full -- that
            // back-pressure is what paces the ROM's transmit loop to the
            // wire.
            if (m_txFifo.size() < kTxFifoMax) m_txFifo.push_back(value);
            m_txEmpty = false;
            m_txReady = m_txFifo.size() < kTxFifoMax;
            return;
        case 0x01: // 21H -- parallel output data == sub-CPU command byte
            m_parallelOut = value;
            // XBUSY rises with /WR; a data write also clears INTP0/INTP1
            // (CPC §8.1, §8.4). /DATA1-8 are inverted, so the sub-CPU sees
            // the complement, strobed in after the DSTB delay.
            m_xbusy = true;
            m_intp0 = m_intp1 = false;
            m_sub.strobe(static_cast<uint8_t>(~value), dstbDelayTStates());
            m_lastSubBusy = m_sub.busy();
            return;
        case 0x02: // 22H -- parameter register (byte -> PR[m_par])
        default:
            writeParameter(value);
            return;
    }
}

void TC8576F::writeParameter(uint8_t value) {
    m_pr[m_par & 0x07] = value;
    switch (m_par & 0x07) {
        case 0x00:
        case 0x01:
        case 0x07: updateCharTStates(); break; // divisor / prescaler
        case 0x05: loadSerialMode(); break;
        default: break;
    }
}

void TC8576F::loadSerialMode() {
    const uint8_t m = m_pr[5];
    // PR5 (CPC §6.1): b0 stop bits, b1 TxINTM, b3:b2 data bits (00 -> 5 ..
    // 11 -> 8), b4 parity enable, b5 even parity, b6 ERINTM, b7 RxINTM.
    m_txIntMask   = (m >> 1) & 0x01;
    m_charLength  = 5 + ((m >> 2) & 0x03);
    m_parityEnable = (m >> 4) & 0x01;
    m_parityEven   = (m >> 5) & 0x01;
    m_errIntMask   = (m >> 6) & 0x01;
    m_rxIntMask    = (m >> 7) & 0x01;
    const WordFormat wf = wordFormat();
    updateCharTStates(wf);

    const uint32_t baud = baudRate(wf);
    if (m_link && baud) m_link->onBaud(baud, wf.bits);
}

TC8576F::WordFormat TC8576F::wordFormat() const {
    const uint32_t divisor = uint32_t(m_pr[0]) | (uint32_t(m_pr[1] & 0x0F) << 8);
    const int stop = (m_pr[5] & 0x01) ? 2 : 1;
    const int bits = 1 /*start*/ + m_charLength + (m_parityEnable ? 1 : 0) + stop;
    return {divisor, bits};
}

uint32_t TC8576F::baudRate(WordFormat wf) const {
    // Baud8x = SYS_CLK / B, B = 0 meaning 4096 and B = 1 stopping the
    // generator; the receiver samples at 8x (CPC §5.2). At the ROM's PR7 = 2
    // this is 76800 / B, the TRM's CWCOM divisor rule.
    if (wf.divisor == 1) return 0;
    const uint32_t b = wf.divisor == 0 ? 4096 : wf.divisor;
    return kXclkHz / (static_cast<uint32_t>(prescaler()) * 8 * b);
}

void TC8576F::updateCharTStates(WordFormat wf) {
    if (wf.divisor == 1) { m_charTStates = 0; return; } // generator stopped
    const long long b = wf.divisor == 0 ? 4096 : wf.divisor;
    // One bit time = 8 * B * K / XCLK seconds.
    const long long t = static_cast<long long>(kTStateHz) * 8 * b * prescaler() * wf.bits / kXclkHz;
    m_charTStates = t < 1 ? 1 : t;
}

void TC8576F::writeControlRegister(uint8_t value) {
    // 23H write, split three ways on D7:D6 (CPC §4.1).
    if (!(value & 0x80)) { if (!m_resetHeld) writeSerialCommand(value); return; }
    if (!(value & 0x40)) { if (!m_resetHeld) writeParallelCommand(value); return; }
    // 11xxxxxx: parameter address. D5 = 1 resets the chip and holds it in
    // reset until a parameter-address write with D5 = 0.
    m_par = value & 0x07;
    if (value & 0x20) {
        resetChip();
        m_resetHeld = true;
    } else {
        m_resetHeld = false;
    }
}

void TC8576F::writeSerialCommand(uint8_t cmd) {
    // CPC §6.2. The ROM keeps a shadow of this register at F14FH.
    m_txEnable  = cmd & 0x01;
    m_dtr       = cmd & 0x02;
    m_rxEnable  = cmd & 0x04;
    m_sendBreak = cmd & 0x08; // SerialLink carries no break condition
    // ERS: clear the error and break flags; a one-shot, not a mode.
    if (cmd & 0x10) m_parityError = m_overrunError = m_framingError = m_rxBreak = false;
    m_rts       = cmd & 0x20;
    if (m_link) m_link->setControl(m_dtr, m_rts);
}

void TC8576F::writeParallelCommand(uint8_t cmd) {
    // CPC §8.3. Any parallel command clears INTP0/INTP1 (§8.4).
    m_intMask1 = (cmd >> 5) & 0x01;
    m_intMask2 = (cmd >> 4) & 0x01;
    m_intp0 = m_intp1 = false;
    switch (cmd & 0x07) {
        case 4: m_prim = true; break;   // PRIME level on: RS-232C
        case 5: m_prim = false; break;  // PRIME one-shot (4.9 us), ends low: SIO
        case 6: m_prim = false;         // PRIME off, clear every flag
                m_xbusy = false;
                break;
        default: break; // 0-3 clear status-line detection flags (not
                        // modelled, see psr()); 7 is a no-op
    }
}

void TC8576F::followParallelHandshake() {
    if (m_xbusy && m_sub.acked()) {
        m_xbusy = false;
        m_intp0 = true;
    }
    const bool busy = m_sub.busy();
    if (m_lastSubBusy && !busy) m_intp1 = true;
    m_lastSubBusy = busy;
}

void TC8576F::tick(int tstates) {
    followParallelHandshake();
    // Standalone (no peer): nothing in the serial side is time-driven.
    if (!m_link) return;

    // Refresh the RS-232C input lines from the peer (seen through a
    // null-modem crossover -- see SerialLink).
    SerialLink::Lines in;
    m_link->getStatus(in);
    m_cts = in.cts;
    m_dsr = in.dsr;
    m_dcd = in.dcd;
    if (in.ri != m_ci) m_sub.setCiLine(in.ri);
    m_ci  = in.ri;

    const long long ct = m_charTStates;
    if (ct == 0) return; // baud generator stopped (B = 1)
    m_serialAccum += tstates;
    // Bound the catch-up so a long scheduler gap can't replay a huge
    // burst of characters in a single call.
    const long long cap = ct * 64;
    if (m_serialAccum > cap) m_serialAccum = cap;

    while (m_serialAccum >= ct) {
        m_serialAccum -= ct;

        // RX: take one byte from the peer while the receiver is enabled
        // (RxEN, CPC §6.2); with it off, bytes wait in the link. An unread
        // previous byte (RxRDY still set) is an overrun.
        uint8_t b = 0;
        if (m_rxEnable && m_link->poll(b)) {
            if (m_rxReady) {
                m_overrunError = true;
            } else {
                m_rxData = b;
                m_rxReady = true;
            }
        }

        // TX: shift one queued byte out while the transmitter is enabled.
        // The chip's /CTS input would gate it (CPC §6.4), but the PC-1600
        // ties /CTS to GND (Service Manual §9-5, printed p. 33), so it is
        // always clear to send. The peer's CS reaches only the ROM, via PSR
        // FAULT, and the ROM does its own gating (SNDSTAT, P2-B6 A524H).
        if (!m_txFifo.empty() && m_txEnable) {
            m_link->send(m_txFifo.front());
            m_txFifo.pop_front();
            if (m_txFifo.empty()) {
                m_txEmpty = true;
                m_txReady = true;
            } else {
                m_txReady = m_txFifo.size() < kTxFifoMax;
            }
        }
    }
}

void TC8576F::reset() {
    resetChip();
    m_resetHeld = false;
}

bool TC8576F::interruptOutput() const {
    // DS §5.9 (CPC §6.5).
    const bool txInt = m_txEnable && m_txReady && !m_txIntMask; // /CTS = 0, see tick()
    const bool rxInt = m_rxEnable &&
        ((!m_rxIntMask && (m_rxReady || m_rxBreak)) ||
         (!m_errIntMask && (m_framingError || m_overrunError || m_parityError)));
    return txInt || rxInt || intF();
}
