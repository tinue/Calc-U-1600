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

### PC-1600 INT is a level
The level is `(intCause() & port 35H) != 0`, driven by `updateIntLine()`. It
isn't an edge or a queued event. The ROM dispatcher reads the causes whatever
the mask is, and the mask only gates INT.

### Absolute-deadline emulation pacing
`Qt6/app/EmulationPacer.cpp` schedules each batch against
`start + n * batch`, not a fresh relative sleep. Relative sleeps oversleep by
about 4% on macOS, and the error builds up into a visible TIME lag.

### RTC keeps running while the LH5803 owns the bus
The RTC accumulator advances in both CPU branches of `PC1600Machine::step()`.
The real RTC sits on the always-powered rail, so TIME keeps running while the
machine is OFF or in auto power-off.

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

### Wording and sources
- **Comments and docs cite original sources only**: TRM, Service Manual,
  ROM dumps and disassembly. Other emulators aren't cited as an authority.
- **Calc-U-1600 is original work.** Don't call it a "fork", and don't call
  other projects "upstream".
