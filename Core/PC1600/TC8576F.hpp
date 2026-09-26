#pragma once
#include <cstdint>
#include <deque>
#include <functional>

#include "PC1600SubCpu.hpp"
#include "../Serial/SerialLink.hpp"

// ── PC-1600 CPC: Toshiba TC8576F (TC8576AF) ──────────────────────────────
//
// The PC-1600's single serial engine. One async RS-232C / SIO channel
// (ART), its baud-rate generator, and a Centronics-style parallel port --
// RS-232C and SIO share the one channel and cannot be used at once. The
// Z-80 (SC-7852) reaches it at I/O ports 20H-27H, selected by `IOSU#`
// (SC-7852 pin 59); the LH-5803 co-processor reaches the same registers
// through its ME1 addresses 0020H-0027H (see LH5803SharedMemory).
//
// **Source.** Toshiba's TC8576AF data sheet (1987 data book pp.159-196),
// summarised with the PC-1600 wiring and the ROM's use of the chip in
// SharpPC1500Reference `PC-1600/PC-1600-CPC-TC8576.md` (cited as "CPC §n").
//
// **Register select** (CPC §3). A1:A0 (= port & 3) pick a register; the
// access direction picks its meaning:
//
//   port  read                  write
//   20H   serial input data     serial output data
//   21H   parallel input data   parallel output data -> sub-CPU command
//   22H   serial status         parameter register PR[address]
//   23H   parallel status       serial command / parallel command /
//                               parameter address, by D7:D6 (CPC §4.1)
//
// **The parallel port carries the sub-CPU link, not a printer** (CPC §8,
// §9.4). A 21H write drives the byte inverted onto /DATA1-8 (the sub-CPU's
// R13-R00), sets XBUSY, and fires DSTB (the sub-CPU's KI) after the
// programmed delay. The sub-CPU's Z10 is the /BUSY input and its Z9 pulse
// is ACK, which clears XBUSY. The answer comes back through I/O 33H (the
// LR38041 buffer), routed straight to PC1600SubCpu::readAnswer() in
// PC1600Memory, not through here. The other parallel status inputs carry
// RS-232C CS/CD/DR, and PRIME selects RS-232C or SIO (CPC §9.5, §9.6).
//
// **Serial peer.** With no `SerialLink` attached (`setSerialLink`) nothing
// is connected: the modem inputs read "off", TxD is accepted and reports
// "sent" immediately, RxD never has data. That is enough for the internal
// handshakes the ROM runs before any medium access (the CE-1601M RAM disk,
// the power-off clock save), and keeps every headless probe deterministic.
//
// **With a peer attached**, `tick()` runs a baud clock derived from PR7 and
// PR1:PR0: it shifts one queued TxD byte onto the link and pulls one RxD
// byte off it per emulated character time, tracks TxRDY/TxEMP/RxRDY/
// overrun, mirrors the peer's CTS/DCD/DSR onto the parallel status bits,
// forwards RI to the sub-CPU's CI input, and forwards our RTS/DTR. XON/XOFF
// and SI/SO are in-band and pass straight through. The transmit queue is
// deeper than the chip's double buffer, so a host bridge doesn't stall the
// ROM's transmit loop; TxRDY drops only when the queue is full.
class TC8576F {
public:
    explicit TC8576F(PC1600SubCpu& sub) : m_sub(sub) { updateCharTStates(); refreshInterruptOutput(); }

    /// Wire the chip's INT output (SC-7852 INT0, cause bit 0). It is a
    /// level, DS §5.9 (CPC §6.5): the unmasked transmit, receive and error
    /// conditions, OR the parallel side's IntF. The hook is called with
    /// the new level whenever it changes. Left unset for standalone tests.
    void setInterruptHook(std::function<void(bool)> hook) { m_intHook = std::move(hook); }
    bool interruptOutput() const { return m_intOut; }

    /// Attach / detach the RS-232C peer. Non-owning -- the host owns the
    /// object and must outlive the chip (or detach first). `nullptr`
    /// restores the standalone no-peer behaviour: queued bytes are dropped,
    /// the transmitter reports ready/empty again and the modem lines fall
    /// back to "nothing connected". A chip reset does NOT clear this.
    void setSerialLink(SerialLink* link);
    SerialLink* serialLink() const { return m_link; }

    /// A read / write of one of ports 20H-27H. `reg` is port & 3.
    uint8_t readRegister(uint8_t reg);
    void    writeRegister(uint8_t reg, uint8_t value);

    /// One emulated-time step, `tstates` SC-7852 T-states. Advances the
    /// sub-CPU parallel handshake and serial timing. Called once
    /// per PC1600Machine::step().
    void tick(int tstates);

    /// The /RESET pin (system reset). PR0-PR7 keep their values (CPC §7).
    void reset();

    /// PRIME output = the gate array's PRIM select: high = RS-232C, low =
    /// SIO (CPC §9.6). Reset and the boot's B6H leave it low.
    bool rs232Selected() const { return m_prim; }

    // ── Debug / test peek ───────────────────────────────────────────────
    uint8_t ssr() const;
    uint8_t psr() const;
    uint8_t parameter(uint8_t i) const { return m_pr[i & 0x07]; }

    /// The DSTB delay Td (CPC §8.1) in SC-7852 T-states: how long after a
    /// 21H write the strobe reaches the sub-CPU's KI.
    int dstbDelayTStates() const;

private:
    uint8_t readRegisterImpl(uint8_t reg);
    void    writeRegisterImpl(uint8_t reg, uint8_t value);
    void    tickImpl(int tstates);
    void    resetImpl();
    // No-peer serial defaults: empty transmitter, modem lines off.
    void    resetSerialState();
    void writeControlRegister(uint8_t value); // 23H write
    void writeSerialCommand(uint8_t cmd);
    void writeParallelCommand(uint8_t cmd);
    void writeParameter(uint8_t value);     // 22H write
    void loadSerialMode();                  // PR5 -> the decoded bits
    /// XBUSY falls on the sub-CPU's ACK; that edge sets INTP0 (CPC §8.4).
    void followParallelHandshake();

    /// SYS_CLK prescaler divisor from PR7 (DS §4.3, CPC §5.1).
    int prescaler() const;

    /// The PR1:PR0 baud divisor and PR5-derived bits-per-character,
    /// shared by loadSerialMode()'s SerialLink::onBaud() notification and
    /// updateCharTStates() below so a future word-format change can't be
    /// applied to only one of the two.
    struct WordFormat { uint32_t divisor; int bits; };
    WordFormat wordFormat() const;
    /// Bits per second for `wf` at the current prescaler; 0 = stopped.
    uint32_t baudRate(WordFormat wf) const;

    /// Recomputes m_charTStates from `wf` -- called whenever the baud
    /// divisor, prescaler or word format changes plus once at construction,
    /// so tick()'s hot path (once per emulated instruction) just reads the
    /// cached value instead of redoing this 64-bit multiply/divide on every
    /// call. Takes wordFormat() by value so loadSerialMode() -- which needs
    /// the same WordFormat for its onBaud() notification -- can compute it
    /// once and pass it in rather than this deriving its own second copy.
    void updateCharTStates(WordFormat wf);
    void updateCharTStates() { updateCharTStates(wordFormat()); }

    PC1600SubCpu& m_sub;
    std::function<void(bool)> m_intHook;
    bool m_intOut{false};
    /// Recomputes the interrupt output and reports a change to m_intHook.
    /// Called after every register access, tick() and reset().
    void refreshInterruptOutput();

    // ── Serial peer ─────────────────────────────────────────────────
    SerialLink* m_link{nullptr};          // non-owning; nullptr => no peer
    std::deque<uint8_t> m_txFifo;         // queued TxD bytes, drained at baud
    static constexpr size_t kTxFifoMax = 512;
    long long m_serialAccum{0};           // T-state accumulator for the baud clock
    long long m_charTStates{0};           // cache -- see updateCharTStates(); 0 = stopped

    // Parameter registers (CPC §4.2), selected by m_par, the address a 23H
    // write of C0H..C7H sets:
    //   0,1 baud-rate divisor B, low / high nibble
    //   2   DSTB delay     3   DSTB width     4   PRIME one-shot length
    //   5   serial mode    6   parallel mode  7   prescaler
    // No documented power-up value; the ROM loads them all.
    uint8_t m_pr[8]{};
    uint8_t m_par{0};
    // A parameter-address write with D5 = 1 holds the chip in reset until
    // the next parameter-address write with D5 = 0 (CPC §7).
    bool m_resetHeld{false};

    // Serial command register (CPC §6.2).
    bool m_txEnable{false};
    bool m_dtr{false};
    bool m_rxEnable{false};
    bool m_sendBreak{false};
    bool m_rts{false};

    // Parallel side (CPC §8).
    bool m_intMask1{true};  // IM1: handshake factor masked (reset state)
    bool m_intMask2{true};  // IM2: status-line factor masked (reset state)
    bool m_prim{false};     // PRIME output level
    bool m_xbusy{false};    // set by the 21H write, cleared by ACK or B6H
    bool m_intp0{false};    // XBUSY fell (ACK arrived)
    bool m_intp1{false};    // BUSY fell
    bool m_lastSubBusy{false};

    // Serial mode (PR5, CPC §6.1) decoded bits.
    bool m_txIntMask{false}; // b1 TxINTM
    bool m_rxIntMask{false}; // b7 RxINTM
    bool m_errIntMask{false}; // b6 ERINTM
    bool m_parityEnable{false};
    bool m_parityEven{false};
    int  m_charLength{5};

    // ── Serial status (22H read, CPC §6.3) ─────────────────────────────
    // b7 DSR (reads 0, see ssr()), b6 RBRK, b5 FE, b4 OE, b3 PE, b2 TxEMP,
    // b1 RxRDY, b0 TxRDY.
    bool m_rxBreak{false};
    bool m_framingError{false};
    bool m_overrunError{false};
    bool m_parityError{false};
    bool m_txEmpty{true};   // no serial peer: transmitter is always drained
    bool m_rxReady{false};  // no serial peer: never any received byte
    bool m_txReady{true};   // no serial peer: always ready to accept a byte

    uint8_t m_rxData{0xFF};
    uint8_t m_txData{0};
    uint8_t m_parallelIn{0xFF};
    uint8_t m_parallelOut{0};

    // ── RS-232C input lines, cached from SerialLink::getStatus() each
    // tick() while a peer is attached. Off with no peer, as with nothing
    // plugged into a real unit.
    bool m_cts{false};      // CS: peer RTS, seen by the ROM only through PSR FAULT
    bool m_dcd{false};      // CD
    bool m_dsr{false};      // DR, seen by the ROM through PSR PE (not SSR b7)
    bool m_ci{false};       // CI (ring), forwarded to the sub-CPU's Q1
};
