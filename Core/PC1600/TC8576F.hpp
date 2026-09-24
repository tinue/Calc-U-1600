#pragma once
#include <cstdint>
#include <deque>
#include <functional>

#include "PC1600SubCpu.hpp"
#include "../Serial/SerialLink.hpp"

// ── PC-1600 UART: Toshiba TC8576F ────────────────────────────────────────
//
// The PC-1600's single serial engine. One async RS-232C / SIO channel
// (ART), its baud-rate generator, and a Centronics-style parallel port --
// RS-232C and SIO share the one channel and cannot be used at once. The
// Z-80 (SC-7852) reaches it at I/O ports 20H-27H, selected by `IOSU#`
// (SC-7852 pin 59); the LH-5803 co-processor reaches the same registers
// through its ME1 shadow of 0x0020-0x0027 (see LH5803SharedMemory).
//
// **Register select.** A1:A0 (= port & 3) pick a register; the access
// direction picks its meaning (PC-1600-IO-Ports.md §3):
//
//   port  read                       write
//   20H   RxD  (serial receive)      TxD  (serial transmit)
//   21H   PIN  (parallel data in)    PVOUT (parallel data out)
//   22H   SSR  (serial status)       parameter register  -> pr[par]
//   23H   PSR  (parallel status)     command register + parameter-address
//
// 21H's write side physically carries the sub-CPU (LU-57813P) command
// byte: on hardware the command travels out through this parallel port,
// which is why the sub-CPU protocol and the UART are one object's problem
// (see PC1600SubCpu's class comment). The matching answer is read at
// I/O 33H (`IOR P`, the LR38041 return buffer) -- routed straight to
// PC1600SubCpu::readAnswer() in PC1600Memory, not through here.
//
// **Provenance.** The register model -- the pr[] layout, the SCR/PCR/SPR
// decode, the SSR/PSR bit order -- is not fully confirmed against the TRM;
// its status-bit *semantics* are being validated against real ROM traces.
// Every bit whose meaning is not yet confirmed against a traced fact is
// marked `TODO(trace)`.
//
// **Scope.** With no `SerialLink` attached (`setSerialLink`) the chip
// keeps its original standalone behaviour: TxD is accepted and reports
// "sent" immediately, RxD never has data. That is enough for the internal
// handshakes the ROM runs before any medium access (the CE-1601M RAM disk,
// the power-off clock save), and keeps every headless probe deterministic.
//
// **With a peer attached**, `tick()` runs a baud clock derived
// from the pr[0]/pr[1] divisor: it shifts one queued TxD byte onto the
// link and pulls one RxD byte off it per emulated character time, tracks
// TxRDY/TxE/RxRDY/overrun, mirrors the peer's CTS/DCD/DSR onto the PSR
// SIO bits, and forwards our RTS/DTR. XON/XOFF and SI/SO are in-band and
// pass straight through. The RS-232C/SIO connector mux and real baud
// hardware remain a later refinement.
class TC8576F {
public:
    explicit TC8576F(PC1600SubCpu& sub) : m_sub(sub) { updateCharTStates(); refreshInterruptOutput(); }

    /// Wire the UART's interrupt output (SC-7852 INT0, cause bit 0). It is
    /// a level: RxRDY while the receiver is enabled and pr[5] b7 leaves RX
    /// interrupts on, OR TxRDY while pr[5] b1 leaves TX interrupts on
    /// (PRRDY/PTRDY: no Centronics device, never set). The hook is called
    /// with the new level whenever it changes. Left unset for standalone
    /// tests.
    void setInterruptHook(std::function<void(bool)> hook) { m_intHook = std::move(hook); }
    bool interruptOutput() const { return m_intOut; }

    /// Attach / detach the RS-232C peer. Non-owning -- the host owns the
    /// object and must outlive the chip (or detach first). `nullptr`
    /// restores the standalone no-peer behaviour: queued bytes are dropped,
    /// the transmitter reports ready/empty again and the modem lines fall
    /// back to their no-peer levels. A chip reset does NOT clear this.
    void setSerialLink(SerialLink* link);
    SerialLink* serialLink() const { return m_link; }

    /// A read / write of one of ports 20H-27H. `reg` is port & 3.
    uint8_t readRegister(uint8_t reg);
    void    writeRegister(uint8_t reg, uint8_t value);

    /// One emulated-time step, `tstates` SC-7852 T-states. Advances the
    /// sub-CPU parallel handshake and serial timing. Called once
    /// per PC1600Machine::step().
    void tick(int tstates);

    void reset();

    // ── Debug / test peek ───────────────────────────────────────────────
    uint8_t ssr() const;
    uint8_t psr() const;
    uint8_t parameter(uint8_t i) const { return m_pr[i & 0x07]; }

private:
    uint8_t readRegisterImpl(uint8_t reg);
    void    writeRegisterImpl(uint8_t reg, uint8_t value);
    void    tickImpl(int tstates);
    void    resetImpl();
    void writeCommandRegister(uint8_t cmd); // 23H write
    void writeParameter(uint8_t value);     // 22H write
    void loadSerialMode();                  // pr[5] -> SO/CL/PEN/...

    /// The pr[0]/pr[1] baud divisor and pr[5]-derived bits-per-character,
    /// shared by loadSerialMode()'s SerialLink::onBaud() notification and
    /// updateCharTStates() below so a future word-format change can't be
    /// applied to only one of the two.
    struct WordFormat { uint32_t divisor; int bits; };
    WordFormat wordFormat() const;

    /// Recomputes m_charTStates from `wf` -- called whenever the baud
    /// divisor or word format changes (writeParameter's cases 0/1/5) plus
    /// once at construction/reset(), so tick()'s hot path (once per
    /// emulated instruction) just reads the cached value instead of
    /// redoing this 64-bit multiply/divide on every call. Takes wordFormat()
    /// by value so loadSerialMode() -- which needs the same WordFormat for
    /// its onBaud() notification -- can compute it once and pass it in
    /// rather than this deriving its own second copy.
    void updateCharTStates(WordFormat wf);
    void updateCharTStates() { updateCharTStates(wordFormat()); }

    /// One emulated character time, in SC-7852 T-states, from the pr[0]/
    /// pr[1] baud divisor and the pr[5] word format. Never zero.
    long long charTStates() const { return m_charTStates; }

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
    long long m_charTStates{1};           // cache -- see updateCharTStates()

    // Parameter register file, selected by m_par (the parameter-address
    // pointer set via a 23H write of C0H..C7H):
    //   0,1 baud-rate divisor lo/hi  (= 76800 / baud, little-endian)
    //   2   DSTB delay / ACK width
    //   3   DSTB width
    //   4   PRIME length
    //   5   serial mode byte (stop/len/parity/int-mask -- loadSerialMode())
    //   6   parallel mode
    //   7   prescaler
    uint8_t m_pr[8]{};
    uint8_t m_par{0};

    // Serial command register (23H write, b7=0) decoded bits.
    bool m_txEnable{false};
    bool m_dtr{false};
    bool m_rxEnable{false};
    bool m_sendBreak{false};
    bool m_rts{false};

    // Parallel command register (23H write, b7:b6=10) decoded bits.
    bool m_intMask1{false}; // b5
    bool m_intMask2{false}; // b4

    // Serial-mode (pr[5]) decoded bits.
    bool m_txIntMask{false}; // b1 set => TxRDY does NOT raise INT
    bool m_rxIntMask{false}; // b7
    bool m_errIntMask{false}; // b6
    bool m_parityEnable{false};
    bool m_parityEven{false};
    int  m_charLength{8};

    // ── SSR (22H read) state ───────────────────────────────────────────
    // Bit order: b7 DSR, b6 RBRK, b5 FE, b4 OE,
    // b3 PERR, b2 TxE, b1 RxRDY, b0 TxRDY.  TODO(trace): confirm which
    // bit the ROM's readiness poll (romIV-6 A8F0/A974) and the LH-5803
    // OFF loop (rom1500 E538, `bii #(0x0023),0x20`) actually gate on.
    bool m_dsr{false};
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
    // tick() while a peer is attached (see PSR SIO overlay in psr()).
    // Default asserted so a line-gated transmit never stalls on a
    // transport that carries no modem lines.
    bool m_cts{true};       // peer RTS  -> our CTS
    bool m_dcd{true};       // carrier detect
    bool m_ci{false};       // ring indicator
};
