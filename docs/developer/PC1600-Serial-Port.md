# PC-1600 virtual serial port

The emulated PC-1600's RS-232C port (TC8576F) is backed by a host
pseudo-terminal (macOS and Linux), so any serial application can talk to
it directly — in particular SharpDataExchange's `sde`. This document is
about the implementation; for using the port, see the User Guide,
[7. COM ports](../User-Guide.md#7-com-ports).

## How it works

**Transport: host pseudo-terminal.** `posix_openpt`/`grantpt`/`unlockpt`/
`ptsname` give a `/dev/ttysNNN` slave that any serial tool can open
like a real port. A stable `calcu1600-rs232c.serial` symlink is the documented
target, so the per-run `ttysNNN` name never leaks to the user. The
symlink's folder is **Settings ▸ Serial ports ▸ Symlink directory**
(default `~/Calc-U-1600`).

**Lifecycle: always active.** The port opens with the PC-1600 machine and
closes with it — there is no enable switch, matching the real hardware's
built-in port. `Qt6/app/MachineController` owns the `PtySerialLink`; a
fresh `/dev/ttysNNN` per machine rebuild is invisible behind the stable
symlink.

**Fidelity: internal line model + software flow-through.** XON/XOFF and
SHIFT-IN/SHIFT-OUT (`SETCOM"COM1:",…,X,N` / `…,N,S`) are in-band bytes
that pass straight through -- there's no device driver on the host side
to intercept them the way a real UART's flow-control layer would, so
XON/XOFF bytes just become part of the data stream as far as the host
app is concerned. In practice this makes `X` handshake tricky to use;
see "Calculator-side settings" below for the recommended alternative (no
handshake, paced with a delay instead). The RS-232C hardware lines are
modelled inside the TC8576F as the Toshiba data sheet and the ROM describe
them ([Ref/PC-1600/PC-1600-CPC-TC8576.md](https://github.com/tinue/Sharp1500-1600-Ref/blob/main/PC-1600/PC-1600-CPC-TC8576.md) §9.5): RTS/DTR
from the serial command register are forwarded via
`SerialLink::setControl()`, and the peer's CS/CD/DR are cached each
`tick()` into the parallel status register, bits 0/1/2. CS reads 0 when on,
CD and DR read 1 (the chip inverts two of the three inputs). The peer's RI
goes to the sub-CPU, which reports it as CI (`ON PHONE`, `WAKE$(1)`). A raw
PTY carries no modem lines, so `PtySerialLink::getStatus()` reports CTS and
DSR permanently asserted (DCD approximates "a peer holds the slave open").
The chip's own /CTS is tied to GND (Service Manual §9-5), so the peer's CS
never holds the transmitter; it only reaches the ROM, which gates in
software when a `SNDSTAT` value asks it to. The receiver only takes bytes while the
ROM has it enabled (RxEN), which it does when a channel is opened; until
then incoming bytes wait in the PTY.

**Baud pacing.** `TC8576F::tick(tstates)` accumulates SC-7852 T-states
and, once per emulated character time (from the PR7 prescaler and the
PR1:PR0 divisor: at the ROM's prescaler, baud = 76800 / divisor), shifts
one queued TxD byte onto the link and pulls one
RxD byte off it. TxRDY drops only when the 512-byte TX FIFO fills, which
paces the ROM's transmit loop to the wire. `PtySerialLink` adds fd-level
back-pressure on the RX side: its reader thread stops draining the
master once its RX ring passes an 8 KB high-water mark, so a blocking
sender stalls — the PTY stand-in for a real RTS drop.

## Calculator-side settings

How to set up the port and exchange programs with SharpDataExchange is in
the User Guide, [7. COM ports](../User-Guide.md#7-com-ports). Why the guide
recommends those settings:

- **No handshake** (`SETCOM …,N,N`), because XON/XOFF bytes pass straight
  through (see *Fidelity* above).
- **`RCVSTAT "COM1:",28`** disables the CTS/CD/DSR handshake checks
  entirely, which is the right choice against `PtySerialLink`: a raw PTY
  carries no real modem lines, so `getStatus()` just hardcodes CTS/DSR
  asserted -- a "must be high" check (`24`, which enables the CTS check)
  passes without testing anything. On real PC-1600 hardware talking to a
  real UART, use `24` (or the matching `SNDSTAT` value) instead, since
  genuine RTS/CTS flow control is meaningful there.
- **`sde --device pc1600emul`** paces the transfer instead of relying on
  XON/XOFF.

## Probe tooling

`tools/pc1600_uart_probe.cpp` (built by `tools/build_pc1600_uart_probe.sh`)
can hold a live port open for a manual transfer:

```
PC1600_SERIAL=1 PC1600_SERIAL_SECONDS=180 \
  ./headless/pc1600_uart_probe roms 'SETCOM"COM1:",9600,8,N,1,N,N;INIT"COM1:",4096'
```

This types the BASIC line, then holds the port open, stepping ~real-time,
for the given number of seconds, printing both the `/dev/ttysNNN` device
and the stable symlink path.

## Known limitations

See `TODO.md` for the still-open items (the exact on-wire `SAVE"COM1:"`
framing and a socket-transport follow-on) and
`docs/developer/PC1600-Core-Limitations.md` for what isn't modelled (the RS-232C/SIO
connector mux, bit-level line timing).
