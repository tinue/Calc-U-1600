#include "TC8576F.hpp"

#include "PC1600Machine.hpp"

// Bit order of the two status registers, kept in one place so ssr()/psr()
// and any future writer agree.
namespace {
constexpr uint8_t kSsrTxRDY = 0x01;
constexpr uint8_t kSsrRxRDY = 0x02;
constexpr uint8_t kSsrTxE   = 0x04;
constexpr uint8_t kSsrPERR  = 0x08;
constexpr uint8_t kSsrOE    = 0x10;
constexpr uint8_t kSsrFE    = 0x20;
constexpr uint8_t kSsrRBRK  = 0x40;
constexpr uint8_t kSsrDSR   = 0x80;

constexpr uint8_t kPsrFAULT = 0x01;
constexpr uint8_t kPsrSLCT  = 0x02;
constexpr uint8_t kPsrPE    = 0x04;
constexpr uint8_t kPsrP5V   = 0x08;
constexpr uint8_t kPsrPRIME = 0x10;
constexpr uint8_t kPsrBUSY  = 0x20; // sub-CPU parallel-port BUSY (Z10 = Ready->Busy->Ready)
constexpr uint8_t kPsrXBUSY = 0x40;
constexpr uint8_t kPsrIntF  = 0x80;

// TC8576F baud generator: pr[0]/pr[1] hold divisor = kBaudRefHz / baud
// (TC8576F.hpp pr[] doc). Character time is scaled into SC-7852 T-states,
// the domain PC1600Machine::step() feeds tick() in.
constexpr uint32_t kBaudRefHz = 76800;
constexpr uint32_t kTStateHz  = PC1600Machine::kTStateHz;
} // namespace

void TC8576F::reset() {
    for (auto& b : m_pr) b = 0;
    m_par = 0;
    m_txEnable = m_dtr = m_rxEnable = m_sendBreak = m_errorReset = m_rts = false;
    m_intMask1 = m_intMask2 = false;
    m_txIntMask = m_rxIntMask = m_errIntMask = false;
    m_parityEnable = m_parityEven = false;
    m_charLength = 8;

    m_dsr = m_rxBreak = m_framingError = m_overrunError = m_parityError = false;
    m_txEmpty = true;
    m_rxReady = false;
    m_txReady = true;
    m_rxData = 0xFF;
    m_txData = 0;
    m_parallelIn = 0xFF;
    m_parallelOut = 0;

    // The peer attachment (m_link) is host-owned and survives a chip
    // reset; only the transient serial state clears here.
    m_txFifo.clear();
    m_serialAccum = 0;
    m_cts = m_dcd = true;
    m_ci = false;
    updateCharTStates();
}

uint8_t TC8576F::ssr() const {
    uint8_t v = 0;
    if (m_txReady)      v |= kSsrTxRDY;
    if (m_rxReady)      v |= kSsrRxRDY;
    if (m_txEmpty)      v |= kSsrTxE;
    if (m_parityError)  v |= kSsrPERR;
    if (m_overrunError) v |= kSsrOE;
    if (m_framingError) v |= kSsrFE;
    if (m_rxBreak)      v |= kSsrRBRK;
    if (m_dsr)          v |= kSsrDSR;
    return v;
}

uint8_t TC8576F::psr() const {
    uint8_t v = 0;
    // The sub-CPU sits behind the parallel port: its BUSY line (Ready ->
    // Busy across a command, back to Ready when the answer is latched) is
    // what a readiness poll between OUT (21H) and IN (33H) watches.
    // TODO(trace): confirm bit position + polarity against romIV-6 A8F0.
    if (m_sub.busy()) v |= kPsrBUSY;
    // FAULT/SLCT/PE/P5V/PRIME/XBUSY/IntF: parallel-printer status, no
    // Centronics device modelled -- all clear.
    //
    // With a serial peer attached, the SIO connector inputs overlay the
    // otherwise-unused low printer-status bits:
    // CS/CD/DS -> b0/b1/b2. Bit set here == line asserted (the INSTAT
    // "high" state). TODO(trace): bit positions + polarity against
    // romIV-6 A8F0 and the INSTAT handler; CI (ring) is not surfaced yet
    // because b5 already carries the sub-CPU BUSY line.
    if (m_link) {
        if (m_cts) v |= kPsrFAULT; // b0 = CS  (clear to send)
        if (m_dcd) v |= kPsrSLCT;  // b1 = CD  (carrier detect)
        if (m_dsr) v |= kPsrPE;    // b2 = DS  (data set ready)
    }
    (void)kPsrP5V; (void)kPsrPRIME; (void)kPsrXBUSY; (void)kPsrIntF;
    return v;
}

uint8_t TC8576F::readRegister(uint8_t reg) {
    switch (reg & 0x03) {
        case 0x00: // 20H -- serial receive data
            m_rxReady = false;
            return m_rxData;
        case 0x01: // 21H -- parallel data in (PIN); no Centronics device
            return m_parallelIn;
        case 0x02: // 22H -- serial status register
            return ssr();
        case 0x03: // 23H -- parallel status register
        default:
            return psr();
    }
}

void TC8576F::writeRegister(uint8_t reg, uint8_t value) {
    switch (reg & 0x03) {
        case 0x00: // 20H -- serial transmit data
            m_txData = value;
            if (!m_link) {
                // No peer: the byte is "sent" at once. Keep TxRDY/TxE set
                // so a polling transmit loop makes progress.
                m_txReady = true;
                m_txEmpty = true;
                if (m_raiseInterrupt && !m_txIntMask) m_raiseInterrupt();
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
        case 0x01: // 21H -- parallel data out (PVOUT) == sub-CPU command byte
            m_parallelOut = value;
            m_sub.command(value);
            return;
        case 0x02: // 22H -- parameter register (byte -> pr[m_par])
            writeParameter(value);
            return;
        case 0x03: // 23H -- command register + parameter-address pointer
        default:
            writeCommandRegister(value);
            return;
    }
}

void TC8576F::writeParameter(uint8_t value) {
    m_pr[m_par & 0x07] = value;
    switch (m_par & 0x07) {
        case 0x00:
        case 0x01: updateCharTStates(); break; // baud divisor lo/hi
        case 0x05: loadSerialMode(); break;
        // 7 prescaler: stored for the baud clock.
        default: break;
    }
}

void TC8576F::loadSerialMode() {
    const uint8_t m = m_pr[5];
    // pr[5]: b0 stop bits, b1 TxINTM, b3:b2 char length (00->5 .. 11->8),
    // b4 parity enable, b5 even/odd, b6 err-int mask, b7 rx-int mask.
    m_txIntMask   = (m >> 1) & 0x01;
    m_charLength  = 5 + ((m >> 2) & 0x03);
    m_parityEnable = (m >> 4) & 0x01;
    m_parityEven   = (m >> 5) & 0x01;
    m_errIntMask   = (m >> 6) & 0x01;
    m_rxIntMask    = (m >> 7) & 0x01;
    const WordFormat wf = wordFormat();
    updateCharTStates(wf);

    if (m_link) m_link->onBaud(wf.divisor ? kBaudRefHz / wf.divisor : 9600u, wf.bits);
}

TC8576F::WordFormat TC8576F::wordFormat() const {
    const uint32_t divisor = uint32_t(m_pr[0]) | (uint32_t(m_pr[1]) << 8);
    const int stop = (m_pr[5] & 0x01) ? 2 : 1;
    const int bits = 1 /*start*/ + m_charLength + (m_parityEnable ? 1 : 0) + stop;
    return {divisor, bits};
}

void TC8576F::writeCommandRegister(uint8_t cmd) {
    // b7 = 0: the low 7 bits are a serial command word (SCR).
    if (!(cmd & 0x80)) {
        m_txEnable   = cmd & 0x01;
        m_dtr        = cmd & 0x02;
        m_rxEnable   = cmd & 0x04;
        m_sendBreak  = cmd & 0x08;
        m_errorReset = cmd & 0x10;
        m_rts        = cmd & 0x20;
        if (m_link) m_link->setControl(m_dtr, m_rts);
    }
    switch (cmd >> 6) {
        case 0x02: // 10xxxxxx -- parallel command register (PCR)
            m_intMask1 = (cmd >> 5) & 0x01;
            m_intMask2 = (cmd >> 4) & 0x01;
            break;
        case 0x03: // 11xxxxxx -- parameter-address set (or chip reset)
            if (cmd & 0x20) reset();
            else            m_par = cmd & 0x07;
            break;
        default:
            break;
    }
}

void TC8576F::updateCharTStates(WordFormat wf) {
    if (wf.divisor == 0) wf.divisor = kBaudRefHz / 9600; // pre-SETCOM default (9600)
    // One bit time = kTStateHz / baud = kTStateHz * divisor / kBaudRefHz.
    const long long t =
        static_cast<long long>(kTStateHz) * wf.divisor * wf.bits / kBaudRefHz;
    m_charTStates = t < 1 ? 1 : t;
}

void TC8576F::tick(int tstates) {
    // Standalone (no peer): the sub-CPU handshake timeline lives in
    // PC1600SubCpu (busy()/answerReady()), advanced separately by
    // PC1600Machine::step(). Nothing in the UART itself is time-driven
    // until a SerialLink is attached.
    if (!m_link) return;

    // Refresh the RS-232C input lines from the peer (seen through a
    // null-modem crossover -- see SerialLink).
    SerialLink::Lines in;
    m_link->getStatus(in);
    m_cts = in.cts;
    m_dsr = in.dsr; // also the SSR b7 (DSR) source
    m_dcd = in.dcd;
    m_ci  = in.ri;

    const long long ct = charTStates();
    m_serialAccum += tstates;
    // Bound the catch-up so a long scheduler gap can't replay a huge
    // burst of characters in a single call.
    const long long cap = ct * 64;
    if (m_serialAccum > cap) m_serialAccum = cap;

    while (m_serialAccum >= ct) {
        m_serialAccum -= ct;

        // RX: take one byte from the peer. An unread previous byte
        // (m_rxReady still set) is an overrun -- SSR b4.
        uint8_t b = 0;
        if (m_link->poll(b)) {
            if (m_rxReady) {
                m_overrunError = true;
            } else {
                m_rxData = b;
                m_rxReady = true;
            }
            if (m_raiseInterrupt && m_rxEnable && !m_rxIntMask) m_raiseInterrupt();
        }

        // TX: shift one queued byte out while the transmitter is enabled
        // and the peer is clear-to-send.
        if (!m_txFifo.empty() && m_txEnable && m_cts) {
            m_link->send(m_txFifo.front());
            m_txFifo.pop_front();
            if (m_txFifo.empty()) {
                m_txEmpty = true;
                m_txReady = true;
                if (m_raiseInterrupt && !m_txIntMask) m_raiseInterrupt();
            } else {
                m_txReady = m_txFifo.size() < kTxFifoMax;
            }
        }
    }
}
