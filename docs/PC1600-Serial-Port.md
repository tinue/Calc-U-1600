# PC-1600 virtual serial port

The emulated PC-1600's RS-232C port (TC8576F) is backed by a host
pseudo-terminal, so any Mac serial application can talk to it directly —
in particular `SharpDataExchange`, which uses jSerialComm and opens a
`/dev/cu.*`-style device.

## How it works

**Transport: host pseudo-terminal.** `posix_openpt`/`grantpt`/`unlockpt`/
`ptsname` give a `/dev/ttysNNN` slave that any Mac serial tool can open
like a real port. A stable `calcu1600.serial` symlink is the documented
target, so the per-run `ttysNNN` name never leaks to the user. The
symlink's folder is configurable in Settings ▸ Serial Port; the default
is the app's own data directory.

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
see "Using SharpDataExchange" below for the recommended alternative (no
handshake, paced with a delay instead). The RS-232C hardware lines are
modelled inside the TC8576F: RTS/DTR from the serial command register are
forwarded via `SerialLink::setControl()`, and the peer's CTS/DCD/DSR are
cached each `tick()` and overlaid on PSR bits b0/b1/b2. A raw PTY carries
no modem lines, so `PtySerialLink::getStatus()` reports CTS and DSR
permanently asserted (DCD approximates "a peer holds the slave open").
The transmitter is gated on CTS but CTS defaults asserted, so a
line-less PTY never stalls `SAVE"COM1:"`.

**Baud pacing.** `TC8576F::tick(tstates)` accumulates SC-7852 T-states
and, once per emulated character time (derived from the `pr[0]`/`pr[1]`
baud divisor), shifts one queued TxD byte onto the link and pulls one
RxD byte off it. TxRDY drops only when the 512-byte TX FIFO fills, which
paces the ROM's transmit loop to the wire. `PtySerialLink` adds fd-level
back-pressure on the RX side: its reader thread stops draining the
master once its RX ring passes an 8 KB high-water mark, so a blocking
sender stalls — the PTY stand-in for a real RTS drop.

## Using SharpDataExchange

On the emulated PC-1600 (once per power-on):

```
SETCOM "COM1:",9600,8,N,1,N,N        ' no handshake -- see the Fidelity note above for why not X
INIT   "COM1:",4096
OUTSTAT "COM1:"
RCVSTAT "COM1:",28
```

then `SAVE "COM1:"` / `LOAD "COM1:"`.

`28` disables the CTS/CD/DSR handshake checks entirely, which is the
right choice against `PtySerialLink`: a raw PTY carries no real modem
lines, so `getStatus()` just hardcodes CTS/DSR asserted -- a "must be
high" check (`24`, which enables the CTS check) would pass trivially
rather than test anything. On real PC-1600 hardware talking to a real
UART, use `24` (or the matching `SNDSTAT` value) instead, since genuine
RTS/CTS flow control is meaningful there.

On the Mac/Linux side, point `SharpDataExchange` at the stable symlink
(SharpDataExchange's own auto-detect only scans `cu.usb*`/`ttyACM*`/
`ttyUSB*`, so the explicit path is required) with its `--raw` option,
which paces the transfer with a fixed delay instead of relying on
XON/XOFF:

```
java -jar SharpDataExchange.jar get --raw -p <path to calcu1600.serial> myprogram.bas
java -jar SharpDataExchange.jar put --raw -p <path to calcu1600.serial> myprogram.bas
```

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

See `TODO.md` for the still-open items (PSR/SSR bit polarity confirmation,
the exact on-wire `SAVE"COM1:"` framing, and a socket-transport
follow-on) and `docs/PC1600-Core-Limitations.md` for what isn't modelled
at all yet (the RS-232C/SIO connector mux, real baud-rate hardware, the
parallel printer port).
