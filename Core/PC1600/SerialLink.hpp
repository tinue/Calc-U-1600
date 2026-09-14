#pragma once
#include <cstdint>

// ── PC-1600 RS-232C peer ────────────────────────────────────────────────
//
// One serial peer attached to the PC-1600's TC8576F. The chip owns a
// non-owning `SerialLink*`; the host owns the object. When no link is
// attached the UART keeps its standalone behaviour (TxD accepted and
// reported "sent" at once, RxD never has data), so every existing test
// and headless probe is unaffected.
//
// **Threading.** Every method here is called only from the emulation
// thread, from inside `TC8576F::tick()` / `TC8576F::writeRegister()`
// (which the GUI runs under `PC1600Machine::m_mutex`). An implementation
// that talks to the host -- a pseudo-terminal, a socket -- does its own
// internal locking between those calls and its I/O threads, exactly like
// the CPU trace rings. It must never call back into Core.
//
// **Flow control.** XON/XOFF and SHIFT-IN/SHIFT-OUT are in-band bytes and
// ride the ordinary `poll()`/`send()` stream -- a link does nothing
// special for them. The RS-232C hardware lines are surfaced separately:
// `getStatus()` reports the peer's outputs (seen through a null-modem
// crossover: peer RTS -> our CTS, peer DTR -> our DSR), `setControl()`
// forwards ours. A transport that cannot carry the modem lines (a raw
// PTY) reports the inputs asserted so a line-gated `SAVE"COM1:"` never
// stalls.
class SerialLink {
public:
    // RS-232C control-signal snapshot, from the peer to us. `true` ==
    // asserted (the INSTAT "high" state).
    struct Lines {
        bool cts = true;  // peer RTS  -> our CTS  (clear to send)
        bool dsr = true;  // peer DTR  -> our DSR  (data set ready)
        bool dcd = true;  // carrier / peer present
        bool ri  = false; // ring indicator, idle
    };

    virtual ~SerialLink() = default;

    /// Non-blocking receive. Returns false and leaves `out` untouched when
    /// the peer has sent nothing.
    virtual bool poll(uint8_t& out) = 0;

    /// Hand one byte to the peer. Called at most once per emulated
    /// character time while the transmitter is enabled and CTS is high.
    virtual void send(uint8_t byte) = 0;

    /// Our DTR / RTS outputs changed (TC8576F serial command register).
    virtual void setControl(bool dtr, bool rts) { (void)dtr; (void)rts; }

    /// Fill `in` with the peer's current line state. Default: all inputs
    /// asserted, no ring.
    virtual void getStatus(Lines& in) { in = Lines{}; }

    /// Advisory: the ROM reprogrammed the baud generator / word format.
    /// A byte-stream transport can ignore it; the emulator's own pacing
    /// is authoritative.
    virtual void onBaud(uint32_t baud, int bitsPerChar) {
        (void)baud;
        (void)bitsPerChar;
    }
};
