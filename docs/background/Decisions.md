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
`PiezoSampler` follows the drive line that the ROM toggles, through each
machine's buzzer gate: PC-1500 NAND(SD0, PC6 · CMT IN) (Service Manual
p.16), PC-1600 18H b6 && b7 && SDO && PB2. So tape traffic sounds as on
the real units (confirmed audible on CSAVE and CLOAD, 2026-10-04); CMT IN
counts as high while no tape interface drives it. It doesn't synthesize
BEEP tones, because the user wants the actual square wave. New sound
sources drive `PiezoSampler::setLevel` from their emulated signal.

### Tapes are fixed-role media, saved at each motor stop
The cassette bay works like the card and disk pickers (TapeManager): a
tape picked from the tape folder's list only plays; one named with the
save button only records, and goes back in to play only by picking it from
the list. That is deliberate: nothing records over a tape picked to load
it. A recording is written (whole) each time the remote relay stops the
motor, so a finished CSAVE is on disk at once and taking the tape out only
un-arms it; while it stays in, further CSAVEs are appended. The tape combo
reacts to `activated`, so picking the tape it already shows still puts it
in to play (rewound).

### The cassette tape moves only while the remote relay runs it
`TapeDeck` advances by emulated CPU cycles, and only while the interface's
remote relay is closed: CE-150 PA1-PA4 pulses (either REMOTE 0 or 1), or
CE-1600P 82H b4/b5. Motor-off time is neither played nor recorded, as with
a CE-152 on the remote jack. The ROM waits ~0.6 s for the motor before it
counts the leader, so a tape needs a long leader (`bin2wav -s 3` or
`-l 0x400`); don't
start the tape before the relay closes to make short leaders work. Because
time is emulated, a recording is the same in real time, turbo or the CLI.

### Tape recordings are AC-coupled at 12 Hz, spikes included
`TapeDeck` records the cassette line through a 12 Hz high-pass at 0.45 of
full scale, primed with the line's average over the first 2 ms. 12 Hz is
measured: a real PC-1600 + CE-1600P `CSAVE`, recorded through a Mac line
input (2026-10-04), decays from a lone step with a 13.8 ms time constant,
and its tones are flat-topped. The real tape has the same lone-step spikes
as ours (the line switched high before the motor, parked low for the 8 s
gap): ~1.8x the tone level, gone within ~30 ms. They are authentic, so
don't filter them out. A 300 Hz corner (tried, from the schematic values)
made them short but drooped the tones, unlike the real unit. Gain 0.45
keeps the 2x steps from clipping; the priming keeps sample 0 silent. The
real leader also starts ~1.6x louder and settles over ~1.5 s; that may be
the recording input's automatic gain, so it isn't modelled.

### LH5811 transmitter: an L write restarts the divider; TD means "sent"
`LH5811Serial` starts a frame at the L write: it restarts the divider and
begins a full start bit at once, and TD is set when the second stop bit is
done. All three come from a real PC-1500 (A01) + CE-150 `CSAVE`
(2026-10-04):
- Back-to-back nibble frames carry exactly 6 mark bits (4 data + 2 stop),
  no extra idle bit: the next frame must start at the write, not at the
  next clock edge.
- The pauses after the header and around the end byte (`TIME_DELAY`, 17
  TP periods) are 78-80 bits of mark. With TD = "L empty" the last frame
  was still being sent during the delay and the pauses came out 11 bits
  (one frame) short.
- The first frame after a pause starts with a whole start bit, its tones
  in phase with the bit. A start bit cut short by a free-running bit clock
  gives the CE-150 reader ERROR 44; tones out of phase give `wav2bin`
  glitches the real tape doesn't have.
The CL1 timer (MSK b7) uses the same divider, but the CE-150 only reads it
while loading, when it doesn't transmit.

### PC-1500 PB7 reads high while ON is pressed
The CE-150's tape reader (LOAD_NIBBLE &BE2F) takes PB7 = 1 as BREAK, IF1
latches PB7's rising edge on a press (TRM p.71), and the PC-1600's
compatible block reads 1 = pressed. The system ROM only sees IF1, so the
polarity matters for the CE-150 alone.

### CE-1600P port 82H reads back its latch
The service manual lists the readback, and the plotter and tape drivers
read-modify-write 82H. Open bus (FFH) would pulse both relay coils and the
CMT-in enable on every plotter Z-motor write.

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
Source: [Ref/PC-1600/PC-1600-SubCPU-LU57813P.md](https://github.com/tinue/Sharp1500-1600-Ref/blob/main/PC-1600/PC-1600-SubCPU-LU57813P.md) §4.1.

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
mask only gates INT. `interruptLevel()` spells out the live bits instead of
calling `intCause()` so that a masked live source (UART, sub-CPU) isn't even
evaluated on this per-step path; keep the two bit assignments in step.

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

### PC-1600 Slot 1 S1-S3 follow Port 3CH b6, in reverse order
`MemorySlotConnector::decode()` drives Slot 1 pins 16/17/18 from the
SC7852's LHS3/LHS2/LHS1, not LHS1/LHS2/LHS3 as the names suggest. Port 3CH
b6 remaps them (TRM SC7852 pins 46-48), and they select bank 0 only:
b6 = 0 gives B800/B000/A800, b6 = 1 gives A000/A800/B000. The boot probe
(P0-B0 03CF) sets 1BH with an empty slot, 1AH for a CE-151 and 5BH for a
CE-155 or a full 16/32 KB module. This order is the only one that fits the
measurements on a real PC-1600: a CE-155 (MEM +8192, 3CH = 5BH, four
separate 2 KB blocks at A000-BFFF) and a CE-155 with pins 4 and 18 taped
off, which is electrically a CE-151 (MEM +4096, 3CH = 1AH, RAM at B000 and
B800 only, BASIC start B0C5H). Don't straighten it to pin 16 = LHS1: the
CE-151 then lands at A800-B7FF.

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
- **`INIT"Sx:","P"` / `"M"` can wipe the S0 program.** When the S0 area moves
  (a slot leaves or joins it), `PRGMOVED` (rom3b 65C8H) empties the S0
  program at the new base.
- **A `bin2wav` tape with its default leader doesn't `CLOAD`** (0.5 s on
  the PC-1500; on the PC-1600 its minimum of ~2.1 s, 6406 cycles). The
  ROM pulses the remote relay, waits ~0.6 s for the motor (PC-1600
  `CASMOTOR` 642EH + `DELAYF0`; the CE-150 likewise), then counts the
  leader: 5000 cycles on the PC-1600 (`CMSYNC`, F1AAH). The ROM's own
  `CSAVE` writes 10000 (PC-1600) or ~8 s (PC-1500). Use `bin2wav -s 3`,
  or `-l 0x400`, which writes the ROM's own gap and leader lengths.
- **`CLOAD -1` doesn't read a PC-1500 tape in MODE 0.** The `-1` is parsed
  and skipped (CE-1600P bank 5 77F6H); only MODE (BMODE b6) picks the tape
  format. Through the CE-1600P, `CSAVE` in MODE 1 is ERROR 110 (66A0H).
- **F127H bit 6 stays set, and F88EH reads 2.** The default `ON TIME$` hook
  sets F127H b6. Only a dispatched `ON TIME$ GOSUB` (P0-B0 3D40H) or a new
  `ON TIME$` time (P2-B6 A85DH) clears it. `RUN` writes TRONMODE F88EH = 2 even with TRON off (P1-B0 5803H).
  Neither affects execution.
- **`MEM` ignores `TITLE`.** `MEM` / `STATUS 0` always reports the S0 area
  (LH5803 $CC30 reads only the S0 pointers), even with `TITLE "S1:"`. The free
  space of an S1/S2 program module is `STATUS 259` / `260`.
- **CE-158 `TERMINAL` / `DTE` drop a byte that arrived before they
  started.** On entry both call `RXCOM` (CE-158 8CBDH) and ignore the
  result; `RXCOM` (81E6H) reads the UART data register on every path, so
  the byte waiting in the receiver is consumed. A peer that sends "AB..."
  before `TERMINAL` runs shows "B..."; bytes that arrive after the call,
  menu phase included, all appear. Same on the PC-1500 and the PC-1600 in
  MODE 1 (checked 2026-10-07 with `--ce158-rx-hold`; the order of events
  traced). In headless runs, hold the peer until `TERMINAL` has started.

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
- **`waitUntilBasicIdle` judges a frame by its share in the command loop**
  (32 PC samples per frame, idle at 3/4), not by one end-of-frame sample.
  The 1/64 s ISR runs at the prompt too; one sample per 60 fps frame hits
  or misses it depending on how the 64 Hz timer lines up with the frame
  grid, and that drifts. After 8858e75 the single sample kept "busy" going
  for 8 s after LH5803 use and made every boot hit its 20 s cap. Don't go
  back to one sample per frame. The prompt is the Z-80 loop in MODE 1 too
  (the LH5803 idles there), so the LH5803 needs no part in the test.
- **The PC-1600 fast loader finishes like the ROM's `LOAD`.** Besides the
  program bytes it writes F867/F02C (or an S1/S2 descriptor end and module
  header), the VARIABLE POINTER check, PRGADR (FE3C-FE41), F89E and F1C1
  (LOADEND, rom3b 70E1H). LIST reads PRGADR, so without it a loaded program
  lists as empty (7b327cd). Checked byte for byte against the typer.
- **The PC-1600 loaders write a module through the slot's pins**
  (`PC1600Memory::slotBusWrite`, a global bank + Z-80 address), not into
  the card image at "address minus window base". A card's image isn't in
  address order: a CE-155 keeps A000H (S1) at 0800H and B800H (its own
  decoder) at 0000H. Found by the loader matrix (dev/loader-matrix/).
  Don't bring back image-offset writes.
- **A PC-1600 module window starts at 8000H, A000H or B000H, nothing
  else.** The ROM's module map (rom3b 67B2-6892) takes the first of
  8000H / A000H / B000H in Slot 1, 8000H / A000H in Slot 2, where
  `MEMORYCHK` finds RAM, and always runs the area up to BFFFH. A module
  whose RAM isn't top-justified (e.g. 8000-9FFF only) is one the ROM can't
  use either, so the loaders need no other geometry. A PC-1500 module using
  YO and S1-S3 together can't work in the PC-1600 at all: pin 4 is RAM2,
  the whole 8000-BFFF window, and with 3CH = 5BH S1-S3 land inside it.
  The CE-151 / CE-155 fit comes from the Slot 1 remap (see "PC-1600 Slot 1
  S1-S3 follow Port 3CH b6").
- **Machine code may run from a module window on past &BFFF** into
  internal RAM, as `BLOAD` and `CLOAD M` write it. In MODE 1 that is one
  LH5803 range, where PC-1500 machine code naturally lives.
- **No line straddles two module banks** (`PC1600ProgramPlacement`). The ROM
  leaves a `00 00` bank-end mark and starts the next bank (LOADSTORE 7074H);
  only ADTBL entry 5 -> internal RAM is contiguous and may be straddled.
  Don't "simplify" placement back to one linear byte stream.
- **`kMaxBasicLineLength` (79)** is a guessed limit on the raw typed line.
  It stays until the real limit is measured.
- **`saveas:` always says `template` or `live`.** There is no default: a
  template is never written again, and a live copy autosaves, so the preset
  author has to pick one. The `file:` form writes exactly the named file.
  That skips the catalog-name checks, and it may overwrite a template file,
  which is how a make preset refreshes its templates. By name, a `template`
  save may replace one of the user's templates, but a bundled name is always
  refused.

### Preset format (docs/background/plans/Preset-Review-Plan.md)
- **A preset is YAML-shaped, not YAML.** The hand-written parser takes flat
  `key: value` fields, `- verb: value` steps and one `text: |` block. Don't
  move the whole file to the YAML reader: a real YAML reader would strip
  ` #1` from `CLOSE #1` and choke on `PRINT "A: B"`. Only `debug:` and
  `bus-rom:` are YAML proper.
- **`type:` is typed exactly as written**, to the end of the line, quotes
  included (`- type: "SAVE LOAD"` types the quotes). No comment stripping,
  no unquoting. A comment after a `type:` step is typed too.
- **One naming rule.** Sharp's product names with the hyphen, in any case
  (`CE-1600P`, `pc-1600`). A device by name or by file: `slot-1:` /
  `slot-1-file:`, `floppy:` / `floppy-file:`; a path-valued key is `file`
  or ends in `-file` (only `host-drive:`, a folder, doesn't). `saveas:`
  uses the same device words (`slot-1`, `slot-2`, `floppy`). Numbers: `&`,
  `0x` or `$` is hex, a bare number is decimal, also in `debug:` and in
  launch configurations' strings (they merge key by key, so one key can't
  have two rules); bare hex is refused, not guessed. Leaving a key out means
  "none"; there is no `none` value. (`after: none` in `debug:` is not an
  exception: it names an action, load without starting, next to `call`
  and `stopOnEntry`.)
- **`debug:` keeps camelCase** (`stopOnEntry`, `cleanStart`) against the
  rest's kebab-case. They are the launch-configuration keys (below), and
  `stopOnEntry` is the DAP name.
- **`program:` has no `format:`.** The file's content picks the loader
  (`Core/ProgramFile`), as for a dropped file; `text: |` or `typed: true`
  types a listing in. Unlike a drop, headerless code needs no `.bin` name:
  the author named the file on purpose.
- **Old forms just fail** as unrecognized (pre-1.0), without pointing at
  the new form. `slot:` in a program block is the exception above.
- **The extension matches the model** (`.pc1500`, `.pc1500a`, `.pc1600`).
  The parser goes by `model:` alone; the extension is for people and the
  file dialog.
- **The parser reads each `program: file:`** to classify it, so a preset
  with a missing file fails before the machine starts, like every other
  parse error.

### Loading programs (docs/background/plans/Loader-Mode-Plan.md)
- **The loaders follow MODE and `TITLE` and never change them.** Load
  BASIC, Load Machine Code, presets and the debugger read BMODE b6 and F1D5H
  at load time; the real machine doesn't switch either, it shows an error.
  A preset switches MODE / `TITLE` itself in `keys:`.
- **A listing is tokenized by MODE.** MODE 1 uses the PC-1500 keyword table,
  so a PC-1500 listing's `CALL` is the PC-1500 token (listed as `XCALL`), as
  after a PC-1500 `CSAVE` + `CLOAD`. This differs on purpose from the ROM,
  whose ASCII `LOAD` and keyboard use the PC-1600 names even in MODE 1.
  PC-1600-only keywords stay plain text in MODE 1 (the tokenizer can't
  know them); only text the PC-1500 can't hold (non-ASCII) is refused.
- **`slot:` is gone from presets and the DAP launch config.** The target
  follows from MODE, `TITLE` and the address. A preset that still has
  `slot:` is refused with an explanation rather than silently ignored.
- **The debugger names the CPU itself** (`cpu` in the launch config): the
  toolchain knows it, so Build & Load doesn't take the MODE's CPU for a
  headerless file. Every other caller does.
- **libsharpdx says what a file is** (`sde_file_info`, wrapped once in
  `Core/ProgramFile`). Don't bring back hand-written header parsing. Its
  `raw-lh5801` / `raw-z80` guess for headerless code never picks the CPU:
  the MODE (or the debugger's `cpu`) decides, never the bytes. The guess
  survives only as `looksLikeCode`, which admits a dropped file. A header
  auto-run of `0` or `FFFF` means no auto-run (the library only knows
  `FFFF`). The library is therefore a required build dependency.
- **A cassette WAV is a program file too.** `Core/ProgramFile` decodes it with
  libsharpdx (`sde_wav_decode`) and describes the file on the tape, so every
  loader (menus, drop, presets) takes WAVs without its own code. This
  fast-load is separate from the cassette bay, where the ROM reads the tape
  in emulated time; the two never share code. A tape with several files:
  the GUI asks which one and hands its image to the loader as bytes;
  everywhere else (presets, a drop's target) the first file counts.
- **A dropped file's content picks the loader** (`Core/DropFile`,
  docs/background/plans/Drag-And-Drop-Plan.md). Presets are recognized by a top-level
  `model:` line, not by extension and not by sde: a preset with a character
  outside the Sharp set isn't `text` to sde. Headerless machine code needs
  both the code heuristic *and* a `.bin` / `.rom` name, because the heuristic
  alone takes JPEGs, PDFs, fonts and Mach-O binaries for code. Drops that
  aren't recognized are ignored without a message, on purpose.
- **A PC-1600 header's bank 0 means "no bank given".** Banks 1-3 are
  honoured exactly (1 = slot 1's upper 16 KB, 2/3 = slot 2), refused when
  that bank has no RAM under the code, and started with `CALL #bank,`.
  Bank 0 can't be told apart from a file that names none, and such files
  are common (e.g. a C program linked at &80C5): they follow the program
  area like `BLOAD` without `#bank`, so they land in bank 2 when slot 2
  holds S0's first run (the ROM fills S0 slot 2 first). Don't make bank 0
  force slot 1: with RAM in both slots (the usual emulator setup -- memory
  costs nothing there) slot 1 is the *middle* of the BASIC area, where
  `NEW "S0:"` can't protect the code; the start of S0 is where it can. On
  real machines slot 1 alone is the common case (a slot 2 card was mostly
  a RAM disk); there S0 starts in bank 0, so reading bank 0 literally and
  auto-detecting give the same place. The two only differ when slot 2
  holds RAM too -- exactly the case where slot 1 can't be protected.
- **Machine code may go into the work area F000-FFFF, with a warning.**
  Many PC-1600 programs live up there, above all in the area of the CE-1F01A
  bar-code reader pen, &FF40-&FFFF (e.g. CLOCK.BIN at &FF3A-&FFFB, which also
  reaches into WAKE$). Don't turn it into a refusal.

### Expansion bus
- **Cards know only the bus.** A peripheral card (CE-150, CE-158, memory
  modules, ...) reacts to the contacts of its plug: `PinState` (40-pin) or
  `SystemBusPins` (60-pin, `Contact60::k…`), with address, R/W, PU, PV and
  the strobes/chip selects. It never knows which calculator it is attached to. Host differences belong in the host's bus model, which
  decides what reaches each pin. Don't give a card host-specific hooks or
  shortcuts. `makeSoftwareDefinedCard(path, host)` doesn't break this: the
  host only checks the file's `compatible-hosts` at load time, like the
  label on the box, and the built card keeps no host.
- **One 60-pin connector per machine, and each card decodes the contacts
  of its own CPU family.** The PC-1600's plug is a single `SystemBus` that
  both CPUs drive (`PC1600BusDrive`). PC-1500-family cards (CE-150, CE-158,
  `BusRomCard`) select on DME0 (56) and ME1/IOE (59); the SC7852-family
  cards (CE-1600P, CE-1600F, host drive, `PC1600BusRomCard`) on MREQ/IORQ and
  the bank bits PT/PU/PVOUT (14/15/16), and ignore every cycle while ELH̄
  (58) is asserted. Don't give the host a per-CPU card list again: which
  card answers which CPU follows from the contacts.
  - DME0 is driven for LH5803 ME0 cycles only. The TRM names contact 56
    DME0, as on the PC-1500, and the CE-150/CE-158 need it; on Z-80 cycles
    its level is unknown and it stays low. Assumed, not measured.
  - The SC7852-family cards' ELH̄ gate is documented for the CE-1600P's ROM
    select (CSNO) only. For their I/O decode it is an assumption that keeps
    the LH5803's ME1 cycles (which drive IORQ) away from ports 70H-9FH. It
    is what blocks a CE-158 + CE-1600P combination (TODO.md).
  - Where a contact's level is unknown (M1 on memory cycles, PT/PU/PVOUT on
    I/O cycles, DME0 on Z-80 cycles), the drive leaves it inactive. Don't
    make a card depend on one of those.
- **Peripheral-ROM fetches take the generic open-bus path. This is fine as
  is.** Every fetch from a card ROM (CE-150 at 0xA000-0xBFFF, CE-158 at
  0x8000-0x9FFF) on the PC-1500 goes through `resolve()` → `readOpenBus()`
  → `SystemBus` → each card's `respondsToRead`. It costs a few calls
  and compares of host time per fetch. It costs no emulated time: LH5801
  cycles come from the opcode tables, and the memory path adds none. The
  ~1.3 MHz guest leaves plenty of host headroom. Don't add a per-card ROM
  pointer or cache. It would break the rule above and gain nothing
  measurable. If profiling ever shows a real cost, the fix belongs at bus
  level and must work the same way for every card.
- **A CE-1600P ROM switch swaps the box. It doesn't reset the machine.** The
  ROM sits in the CE-1600P, so picking the other version on an attached one
  counts as unplugging one CE-1600P and plugging in another. The switch runs
  through the attach/detach OFF/ON cycle (`MachineController::swapCE1600PRom()`).
  RAM survives. The floppy goes into the new drive with the same side up, and
  the changed-disk latch is armed. The PC-1600's own ROM switch still rebuilds
  the machine, because that ROM is in the calculator.
- **The host-directory drive lives in page-1 bank 7 and loses `Y:` to the
  CE-1600F.** Bank 7 is where the MEP rev3 module sits on the real 60-pin
  bus. FILE_I searches modules in bank order, so with a CE-1600P attached
  `Y:` is the floppy's second drive and only `S3:` reaches the host folder.
  Don't move it to bank 2 to win `Y:` (docs/PC1600-Host-Drive.md).
- **The host drive's protocol is our own, not the MEP's.** The ROM ships the
  FCB header over ports 90H/91H and takes back status, ERL, FCB and DMA data.
  All file logic stays in `HostDirectoryDrive`, where it can be tested. The
  MEP ROM is a guideline only, and isn't copied or emulated.
- **The host drive matches the MEP's public interface, not its protocol.**
  The fixed entries 4020H/4023H/4026H, the tokens F2D0H/F2D1H (CDIR/LDIR)
  and the prompt buffer FB10H are the MEP's, because MEP software (FILEX)
  calls them by address without any check. It crashed on the device table
  that used to sit at 4020H. Don't move the device table back below 4029H.
- **S3 stays a superset of the MEP.** APPEND, DSKF, SET, GET LENGTH and
  several open files keep working, although a real MEP answers ERROR 158 or
  allows one file per direction. MEP software doesn't depend on those errors.
- **Host-drive subdirectories:** `..` at the top folder stays there; DIRMODE
  lists no `.`/`..` (FILEX builds its own path); directory entries have
  attribute 00H, because FILES hides 10H and LDIR is FILES; symlinked folders
  are hidden. The current directory resets on power-on, resume after APO and
  reset only, not on NEW (docs/PC1600-Host-Drive.md).
- **LDIR's trampoline lives on the stack, not in LISTBUF** as on the MEP: a
  direct command is tokenized in LISTBUF (FBB0H).
- **Host-drive WRITE stores whole records; CLOSE trims.** That is the
  CE-1600F's model (FDWRITE/FDCLOSE). BASIC and COPY both rely on CLOSE
  trimming the last record to FCB+06H bytes. Writing only FCB+06H bytes per
  record would trim twice.

### Typing into the machine
- **The GUI Paste Text never presses ENTER.** This is deliberate: a careless
  paste must not run anything. Tool paths (debugger auto-start, future inbound
  APIs) use `MachineController::typeCommand()`, which does press ENTER.
- **PC-1600 accented characters are typed through KBII, one sequence per
  character.** A character from the ROM's KBII tables (`KYCDKB2` /
  `KYCDSK2`, P2-B6 9592H / 95E5H) is typed as KBII, [SHIFT,] key, KBII.
  - Every path types what was typed: `é` gives é and `É` gives É, on host
    keys too. This breaks deliberately with the host letters (`g` gives G):
    their uppercase default is for BASIC keywords, and accented characters
    only ever appear in strings and REMs.
  - The sequence checks what the user has latched when it starts (SYMB0
    F64EH bit 1 SHIFT, STAT2 F3C6H bit 7 KBII). With SHIFT latched it taps
    SHIFT first to un-latch it: with SHIFT on, the KBII key toggles the key
    click instead (EDKBII P1-B0 6CB3H). With KBII latched, with or without
    SHIFT, the character is dropped silently, so the user's KBII stays as
    it is.
  - SHIFT+KBII is not always the lowercase of the KBII character: for 12
    keys it is another character (B ù/û, H ¡/½, ( ₧/«, …). Those, and an
    uppercase the ROM lacks (Ë, Û → the lowercase), are typed as they are,
    with no attempt to be smart.
  - Dead keys go through the OS input method (`MainWindow` has
    `WA_InputMethodEnabled`), not a table of our own: the composed character
    arrives as the commit string and is typed as a tap. On macOS the
    press-and-hold accent picker is switched off for the app, so a held
    letter stays a held key.
  - A run like `öäü` is not batched under one KBII latch. It would save two
    taps per adjacent accented character in paste / `type:` only, but every
    exit path (cancel, untypeable character) would have to release the
    latch. A self-contained sequence always leaves KBII off.

### GUI
- **Hardware pickers (model, memory modules) live on the control bar**, not
  in Settings. The picks reset on a model switch and aren't saved. Settings
  holds app-level preferences only.
- **ROM versions and the "foreign" peripherals are in the Machine menu
  only**, to keep the control bar narrow; they rarely change. The bar has
  buttons only for each model's own peripherals (CE-150 + CE-158 on a
  PC-1500/1500A, CE-1600P on a PC-1600); Machine ▸ Peripherals has all of
  them. The tape counter doubles as the motor lamp (green while it runs).
- **Mount Directory is in the File menu**, not on the control bar, at the
  user's request. Like the hardware pickers, it isn't saved across launches.
- **The app registers `ApplePersistenceIgnoreState = YES`**
  (`Qt6/app/MacAppSupport.mm`). Without it, the macOS "reopen windows?" prompt
  deadlocks the synchronous load of the startup preset.
- **Copy Screen's text is plain Unicode, while `--lcd-text` escapes**
  (`\\`, `\xHH`, U+FFFD; `Core/Display/LcdText.hpp`). The text is for
  reading, so its glyphs appear as themselves and graphics as blanks. The
  test/DAP form must stay unambiguous, so don't unify the two. The plain
  text comes from each glyph's drawn shape, not its code, because the fonts
  draw some ASCII codes as other signs (PC-1500 5BH √, PC-1600 `CGSPEC`).

### File formats
- **Every file format the app reads carries a required `format-version`**
  (cards, floppies, presets). A reader checks it before the key list and
  refuses a missing or unknown version instead of guessing. A file without
  it fails like any other unreadable file: a saved card without the line
  drops out of the module picker without a message, on purpose (no
  migration before 1.0).
- **Floppies are `.floppy.yaml` only.** `.floppy.img` isn't read, and no
  backward compatibility is wanted. Any format change bumps `format-version`.
- **Cards and floppies resolve bundled-first**, then the configured save
  folder, in both the GUI and the preset loader.
- **`INIT "S3:"` is refused.** The host-drive ROM has no token table, so INIT
  finds no handler. A format must never wipe a host folder.
- **Host-drive directory dates say 1986.** The PC-1600 clock has no year, so
  the ROM's own FATTIME writes year field 6, and the host drive does the
  same. FILES shows no year. The host file keeps its real time stamp.
- **The host drive shows only 8.3 names** the ROM's parser accepts, and
  stores bytes unchanged: there is no line-ending conversion.
- **The slot-record layout stays additive** for the planned battery-backed
  module split. Reserve the `batteryBacked` flag and keep
  `ExpansionCard` serialize/deserialize as the single seam for it.
- **Serial port files are named after their connector:**
  `calcu1600-rs232c.serial`, `calcu1600-ce158.serial`, and later
  `calcu1600-sio.serial`. One file per connector; a file carries data only
  while its connector is selected (PRIME), like a cable in the other
  socket. No bare `calcu1600.serial` and no alias for an old name.
  SharpDataExchange's `pc1600emul` device appends the RS-232C name itself,
  so a rename goes into both repositories.

### Repository layout
- **`examples/` is user-facing only.** It ships as the release's examples
  zip, so it holds material for learning or using the emulator, grouped by
  topic and listed in `examples/README.md`. Hardware-verification programs
  go to `dev/hardware-checks/`, debug and regression presets to
  `dev/presets/`, the VS Code "Debug current file" presets to
  `vscode/calcu1600-debug/presets/` (they ship with the extension).
- **Tests don't read from `examples/`.** They use copies under
  `Core/tests/fixtures/` (e.g. `memtest_stock.bin` next to its `.rst`), so
  renaming or editing an example can't break a test. The duplicate binary is
  deliberate.
- **`memory_card_tests.cpp` keeps inline card definitions** (`kCe1601mYaml`,
  `kSuperRamYaml`, ...) next to the bundled `Qt6/resources/cards/` files.
  They are engine fixtures, not stale copies: the bundled superRAM, for
  one, is a formatted RAM-disk image, so a "fresh bank" isn't blank there.
  Tests about the shipped modules use `bundledCard()`.
- **Tests type only what they test.** Typing costs ~0.13 s emulated per
  character, so a program that is only setup goes in through the fast
  loader (`loadBasicProgram`), and the typer is used where typing or the
  ROM's own tokenizing is the subject (e.g. the host drive's `CDIR`/`LDIR`,
  which libsharpdx doesn't know). Likewise, a test that applies a PC-1600
  preset loads the ROMs first: an empty bus "boots" too, but runs into the
  idle-wait caps and takes several times longer.
- **`Core/tests/LegacyExpression.hpp` is a test oracle.** It keeps the old
  interpreted debugger expression evaluator, hit conditions and log
  interpolation, and the tests check the compiled forms against it. The
  duplication is deliberate.

### Debugger IDE integration (docs/background/plans/Debugger-Use-Cases-Plan.md)
- **Nothing is copied into a project for the generic cases.** The VS Code
  extension carries the "Debug current file" / "Reset and stop"
  configurations, the builds and the default presets, installed per user.
  Don't bring back workspace templates to copy (the old `vscode/workspace/`).
- **Per-project settings live in the project preset's `debug:` block, not
  in `launch.json`.** CLion's DAP settings are per IDE, and a preset is
  something the app can load by hand too; `launch.json` only points at it
  (`project`) and says what to build. The app ignores `debug:` when loading
  the preset.
- **ROM extensions go in through preset keys (`bus-rom:`, `slot-N-rom:`),
  not app command-line options.** Every clean start re-reads the preset and
  the files, so Build & Load needs no app restart and the debug session
  stays attached.
- **A card file never points at another file.** `encoding: file` is gone:
  a card is one self-contained file. A ROM module under development keeps
  a placeholder ROM (`$0000: FF...`) and the preset's `slot-N-rom:`
  replaces its bytes. That only works for a single-region ROM module of
  exactly the file's size, and such a card is never saved back (its file
  doesn't hold that ROM).
- **A bus ROM shadows a bundled ROM at the same place** (it is attached in
  front of the chain). On real hardware that would be a bus conflict; here
  it is how a rebuilt ROM replaces a bundled one (the host drive's bank 7,
  the CE-158's ROM) while the device's own I/O stays.
- **`boot: debug` skips the preset's `keys:` and refuses `program` /
  `command`.** Breakpoints must never be armed while a preset or loader
  drives the machine (those loops would hang); so the preset only arms the
  machine, and the debugger runs the whole boot itself.
- **The extension runs `build` itself before the session, instead of
  `preLaunchTask`.** Build & Load needs the same build, and a
  configuration generated for "the current file" can't name a task label.
- **Snapshots are pre-execution everywhere.** The live frame, the history
  frames 1–20 and every `TRACE.bin` frame show the registers an instruction
  started from, not its result. An instruction's effect is in the next newer
  frame (post(N) = pre(N+1)). Post-execution history made frame 1 a copy of
  frame 0 and gave the highlighted line two meanings. Don't switch any of
  them back.

### Wording and sources
- **Comments and docs cite original sources only**: TRM, Service Manual,
  ROM dumps and disassembly. Other emulators aren't cited as an authority.
  Other people's non-primary material (disassemblies, analyses, dumps) is
  credited in THIRD-PARTY-NOTICES.md's acknowledgments, not inline.
- **The research repository is cited as `Ref/<path>`**, a path inside
  [Sharp1500-1600-Ref](https://github.com/tinue/Sharp1500-1600-Ref). The
  full link is in docs/developer/README.md; Markdown files link each
  citation.
- **Docs and comments describe what is, not how it got there.** Fix
  stories, "used to", plan phases and commit hashes go in commit messages
  and CHANGELOG.md. This file is the exception: it may name the commit
  where something was tried and reverted. A handoff is deleted once its
  work is resolved, after its lasting facts move to the code, Decisions or
  the developer docs.
- **No FILEX guide or binary in this repository.** There is no agreement
  with its author. FILEX is named only as MEP software that runs on `S3:`.
- **Calc-U-1600 is original work.** Don't call it a "fork", and don't call
  other projects "upstream".
