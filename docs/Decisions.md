# Decisions

Things that look wrong, redundant or over-complicated at first sight, but are
deliberate. Simplification passes and code reviews keep coming back to them.
Check this list before you "fix" or remove any of them. When a review settles
a question like this, add an entry here.

Each entry says what stays, why, and where it lives.

## Emulation fidelity: keep as is

### CE-150 carriage constants are in half-step units
`kCE150TurretArmX = -32` and `kCE150LeftStopX = -90`
(`Core/Connector/AlpsPlotterMechanism.hpp`) are twice the "obvious" values
-16 / -45. `m_penX` counts half steps (±1 per half step, ±2 per full step),
so the clamp and arm thresholds must be in the same units. With -16 / -45,
homing overshoots and every CE-150 plot is shifted about 4.5 mm to the right.
- Don't make the CE-150 path drop half steps. The firmware's homing and print
  loops depend on half-step moves, and the PC-1500 freezes at CE-150 attach
  without them (tried in 6e9ce55, reverted in 5689ed2).
- Don't correct the offset in the paper view instead. Every consumer of
  `penX` (for example `colorMagnet()`) would still get the wrong value.

### `colorMagnet()` is a computed level, not a latch
It is computed from the current state and is not a sticky flag that
remembers the carriage moved. The PC-1600 boot turret self-test hangs
without this (958f6d1).

### CE-150 ROM window is gated on PV = 0
`Ce150Card` serves its ROM only for ME0 reads at 0xA000-0xBFFF with PV = 0
(`Core/Connector/Ce150Card.hpp`). The gate carries load. PC-1500 ROM A04 sets
PV = 1 to bank RAM into that window around the edit buffer. Without the gate
the card hides that RAM, and every BASIC line is rejected (regression 931618e,
fixed in b909f62). The card behaves the same on every host. Only the host
decides what reaches the PV pin.

### LH5803 internal PIO latch at ME1 0xF000-0xF00F
`LH5803SharedMemory` decodes it with the narrow `(addr & 0xFFF0) == 0xF000`
mask, before falling through to ME0. Don't use the PC-1500's broad
`(addr & 0x3000) == 0x3000` chip select: on this bus it would swallow ME1 RAM
and card accesses. The latch is a plain 16-byte latch and deliberately not the
full named register block.

IF (0xF00B) is software-only. It isn't cleared on read and gets no live
TP/RTC edge. The firmware clears the bit itself, and a stray edge in the
middle of a loop breaks CE-150 LPRINT/TEST on the PC-1600. The user chose this
over a live edge (a34f57d).

### SC7852 adds one wait state per M1
`kM1WaitStates` (`Core/CPU/SC7852/SC7852.cpp`) looks like an arbitrary
slowdown. It is measured: BEEP pitch on the real unit matches to 0.22%, and it
agrees with the TRM formula.

### PC-1500 power-up RAM: &7600-&7BFF reads $FF
`PC1500Memory::clearRam()` fills display RAM plus the first 1K of system RAM
with $FF and everything else with $00. This comes from measurements on real
hardware. The ROM never writes LOCK ($79FF) on reset or CL. Don't
"simplify" it to one fill value.

### uPD1990AC TP runs freely off the divider
`Upd1990ac::tpLevelNow()` takes the phase from the divider and doesn't reset
it on commands. On the real unit, BEEP repeats are whole 64ths of a second.

### Buzzer audio follows the drive line
`PiezoSampler` follows the drive line that the ROM toggles (PC-1500 OPC b6;
PC-1600 port 18H b6 && b7 && SDO). It doesn't synthesize BEEP tones, because
the user wants the actual square wave. New sound sources drive
`PiezoSampler::setLevel` from their emulated signal.

### HLE sub-CPU answers IOCS 25H
The HLE sub-CPU answers the IOCS 25H probe (4FH/4EH → AAH/55H) and responds
in 1.66 ms. Without the answer, every OUT (21H) waits for a PB5 edge and the
0.5 s interrupt handler becomes 8-16 ms long.

### PC-1600 power-off is a real power cut; power-on is a reset
Once the sub-CPU has the system-off command (operand EAH, sent raw as 15H by
the LH-5803's OFF routine) and the bus owner halts, `PC1600Machine` stops
stepping the CPUs and only advances the always-on clocks (`m_poweredOff`).
The ON key, the wake-up timer (SWPON bit 1) and CI (SWPON bit 0) switch it
back on through an ordinary reset that reports the cause via A5H. The boot ROM
then resumes through the FA08H signature, or runs the WAKE$ command string.
- Don't bring back "ON resumes the halted CPU". WAKE$(0)/(1) and the ROM's
  own resume logic only work through the reset.
- `m_rtcAccum` isn't touched by any reset or power cycle: it's the
  sub-CPU's divider.
Source: SharpPC1500Reference PC-1600-SubCpu-LU57813P.md §4.1.

### Sub-CPU SRIRQ clears on read; INT6 is a level
`PC1600SubCpu` keeps events as pending bits. SRIRQ (A2H) returns and clears
all of them, and port 32H bit 6 is `(pending & SWMSK) != 0`, live. The ROM's
INT6 handler keeps the masked-off bits in F07EH itself, which only makes
sense if the read clears them (P1-B3 419FH). The RAM disk was checked with
this model (CE-1601M SAVE/NEW/LOAD).

### Sub-CPU commands without an answer ACK on receipt
Z9 pulses at the end of the window for commands with an answer and on
receipt otherwise, while Z10 stays busy for the whole window (Service Manual
§4-3, type (i)/(ii)). Both CPC flags (PSR b5 BUSY, b6 XBUSY) follow from it.

### TC8576F receiver ignores the line while RxEN is off
With RxEN = 0 the link isn't polled, so bytes wait in the host PTY instead of
latching into RxD (data sheet: the receiver is disabled). Nothing is lost that
the ROM would have read: it enables the receiver when it opens a channel.

### TC8576F PSR bit 0 reads 0 when CS is on
FAULT isn't inverted by the CPC but /SLCT and /PE are, so CS reads 0 when on
while CD and DR read 1. The ROM flips only bit 0 (P2-B6 A526H `XOR 01H`).
It looks inconsistent, but it is the hardware.

### TC8576F transmits whatever the peer's CS says
The chip's /CTS pin is tied to GND and its /DSR pin to RXD (Service Manual
§9-5, printed p. 33), so CS never holds the transmitter, TxRDY or the Tx
interrupt, and SSR bit 7 isn't the peer's DSR. The peer's CS and DR reach
only the ROM, through PSR FAULT and PE. The ROM does the flow control itself
(`SNDSTAT`, P2-B6 A524H). Don't gate `tick()`'s transmit on `m_cts`.

### PC-1600 INT is a level
The level is `(intCause() & port 35H) != 0`, computed by
`PC1600Memory::interruptLevel()` when the SC7852 asks for it at the start of
each `step()`. It isn't an edge or a queued event, and no device pushes
updates. The ROM dispatcher reads the causes whatever the mask is, and the
mask only gates INT.

### Absolute-deadline emulation pacing
`Qt6/app/EmulationPacer.cpp` schedules each batch against
`start + n * batch`, not a fresh relative sleep. Relative sleeps oversleep by
about 4% on macOS, and the error builds up into a visible TIME lag.

### RTC keeps running while the LH5803 owns the bus
The RTC accumulator advances in both CPU branches of `PC1600Machine::step()`.
The real RTC sits on the always-powered rail, so TIME keeps running while the
machine is OFF or in auto power-off.

### PC-1600 LCD: stale first data read, fitted busy time
`PC1600Display` follows the HD61102 datasheet. A data read returns the output
register that the *previous* read loaded, so the first read after setting an
address is stale. The ROM discards that dummy read (bank 6 `81E8H`). The
status byte reports display-off in bit 5, and busy doesn't clear while CK0
(port 37H bit 4) is off.
- `kBusyClocks = 4` is a fit to real-unit benchmarks, not a datasheet
  figure. It is inside the datasheet's 1–3 φ cycles: CK0 drives the
  HD61203's CR pin (Service Manual key circuit diagram, printed p. 43),
  which halves it into φ. Don't "correct" it to the datasheet min or max.
  The re-fit plan is in TODO.md.
- The controllers aren't reset on power-on or reset: VGG keeps their RAM and
  registers. Only CK0 stops.

## Authentic ROM behaviour: not bugs

- **CE-1600P Y clip at about 1000 units.** This is the `PAPER` default (999
  for roll paper, 30 for cut sheets), anchored at the PINIT origin. `SORGN`
  doesn't reset it. A program that needs a taller window uses `PAPER n`.
- **`NEW "S1:"` / `NEW "S2:"` → ERROR 24 on extension memory.** The slot
  must hold a program module (55h header, created with `INIT"Sx:","P"`),
  checked at romI-0 1EC7 and rom3b 439C.
- **`INIT"S2:","P",n` or `"M"` on a virgin CE-1601M doesn't format it.**
  Run `INIT"S2:","F"` first (rom3b 6464 gate on F421 bit 4).
- **RAM-disk INIT caps at media F8 (256 KB).** Larger volumes, such as the
  superRAM 512K, are hand-patched boot sectors, the same approach as the real
  `SUPERRAM.BIN`.
- **F89B = 160 right after boot** is left over from the boot drive scan. It
  doesn't signal a failed command.

## Accepted limitations: won't fix

### Plotter pen colour can drift after OFF/ON
Symptom: set `COLOR 2`, switch off and on, print again, and the CE-1600P or
CE-150 draws in the wrong colour. It is typically off by a fixed amount from
then on. A plain reset is fine; only OFF/ON causes the drift.

Why it happens: the pen colour is state in firmware RAM, and the CPU has no
colour-home signal to read. On the real plotter, colour 0 is a mechanical
detent that a blind fixed-count turret spin reaches during the OFF→ON
re-init. `AlpsPlotterMechanism` just counts turret clicks, so that spin
doesn't reliably land on colour 0.

A real fix would have to detect exactly that init spin (carriage homing and
turret rotation together, which no ordinary `COLOR n` or `LPRINT` does) and
force colour 0 at its end. Do **not** reintroduce a bare "N turret clicks with
pen up ⇒ home spin" heuristic. It fires in the middle of `COLOR` commands and
breaks ordinary colour changes (tried once and reverted).

### TIME advances at emulated-CPU rate, not wall-clock
The RTC is seeded once from the host clock. After that, only emulated CPU
cost advances it; it is never re-synced, so it runs ahead whenever the
emulator runs faster than real time. This is intended: a `T1=TIME … T2=TIME`
bracket measures emulated work and gives the same result at any speed. A
preset that needs wall-clock-accurate `TIME$=` / `DATE$=` must run at
authentic speed for the span that matters.

## Design choices

### Preset loading
- **Fix the loader, not the preset.** If an existing preset breaks after a
  loader or emulator change, fix the Core loader. Don't add `- wait:` or other
  workaround steps to the preset.
- **The preset owns `NEW0` and the mode.** The BASIC loaders don't run `NEW0`
  or switch RUN/PRO. They only validate BASPRG_ST, and the error message says
  "run NEW0 first".
- **Hardcoding the PC-1600 idle PC ($92xx)** in `waitUntilBasicIdle` is fine,
  because the ROM set is fixed. "Small stable PC span" alone isn't enough:
  INPUT, plot and FOR/NEXT waits are tight loops too.
- **The PC-1600 fast loader writes PRGADR ($FE3E-$FE41)** in addition to the
  BASIC pointers. LIST reads it, so without it a loaded program lists as empty
  (7b327cd).
- **`kMaxBasicLineLength` (79)** is a guessed limit on the raw typed line.
  It stays until the real limit is measured.

### Expansion bus
- **Cards know only the bus.** A peripheral card (CE-150, CE-158, memory
  modules, ...) reacts to the connector signals in its `PinState`: address,
  ME0/ME1, R/W, PU, PV and chip selects. It never knows which calculator it
  is attached to. Host differences belong in the host's bus model, which
  decides what reaches each pin. Don't give a card host-specific hooks or
  shortcuts. `makeSoftwareDefinedCard(path, host)` doesn't break this: the
  host only checks the file's `compatible-hosts` at load time, like the
  label on the box, and the built card keeps no host.
- **Peripheral-ROM fetches take the generic open-bus path. This is fine as
  is.** Every fetch from a card ROM (CE-150 at 0xA000-0xBFFF, CE-158 at
  0x8000-0x9FFF) on the PC-1500 goes through `resolve()` → `readOpenBus()`
  → `SystemBus` decode → each card's `respondsToRead`. It costs a few calls
  and compares of host time per fetch. It costs no emulated time: LH5801
  cycles come from the opcode tables, and the memory path adds none. The
  ~1.3 MHz guest leaves plenty of host headroom. Don't add a per-card ROM
  pointer or cache. It would break the rule above and gain nothing
  measurable. If profiling ever shows a real cost, the fix belongs at bus
  level and must work the same way for every card.

### Typing into the machine
- **The GUI Paste Text never presses ENTER.** This is deliberate: a careless
  paste must not run anything. Tool paths (debugger auto-start, future inbound
  APIs) use `MachineController::typeCommand()`, which does press ENTER.

### GUI
- **Hardware pickers (model, memory modules) live on the control bar**, not
  in Settings. The picks reset on a model switch and aren't saved. Settings
  holds app-level preferences only.
- **The app registers `ApplePersistenceIgnoreState = YES`**
  (`Qt6/app/MacAppSupport.mm`). Without it, the macOS "reopen windows?" prompt
  deadlocks the synchronous load of the startup preset.

### File formats
- **Floppies are `.floppy.yaml` only.** `.floppy.img` isn't read, and no
  backward compatibility is wanted. Any format change bumps `format-version`.
- **Cards and floppies resolve bundled-first**, then the configured save
  folder, in both the GUI and the preset loader.
- **The slot-record layout stays additive** for the planned battery-backed
  module split. Reserve the `batteryBacked` flag and keep
  `ExpansionCard` serialize/deserialize as the single seam for it.

### Repository layout
- **`examples/` is user-facing only.** It ships as the release's examples
  zip, so it holds material for learning or using the emulator, grouped by
  topic and listed in `examples/README.md`. Hardware-verification programs
  go to `dev/hardware-checks/`, debug and regression presets to
  `dev/presets/`, the VS Code "Debug on ..." presets to `vscode/presets/`.
- **Tests don't read from `examples/`.** They use copies under
  `Core/tests/fixtures/` (e.g. `memtest_stock.bin` next to its `.rst`), so
  renaming or editing an example can't break a test. The duplicate binary is
  deliberate.

### Wording and sources
- **Comments and docs cite original sources only**: TRM, Service Manual,
  ROM dumps and disassembly. Other emulators aren't cited as an authority.
- **Calc-U-1600 is original work.** Don't call it a "fork", and don't call
  other projects "upstream".
