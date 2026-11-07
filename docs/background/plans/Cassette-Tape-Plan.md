# Cassette CLOAD / CSAVE via WAV files

> **Status (2026-10-04): implemented for the PC-1500/1500A + CE-150 and the
> PC-1600 (MODE 0) + CE-1600P.** How the work differed from the plan below:
> - PC-1600 MODE 1 `CLOAD` through the CE-1600P is **not** done. Its reader
>   (`CM1500BIT` 62BDH) times bits against MSK 1AH b7, and no PC-1600 ROM
>   bank ever writes G (19H). So the PC-1500's CL1 = CL0 wiring doesn't
>   explain that bit on the SC-7852. That is an open hardware question; see
>   TODO.md. The CE-150 on a PC-1600 stays without a tape path, as planned.
> - The LH5811 serial block (`Core/CPU/LH5811Serial.hpp`) is used by the
>   PC-1500 only. The PC-1600's measured F-register modulator stays as it
>   was.
> - **PB7 polarity fixed:** the PC-1500's ON key now reads high while
>   pressed. The CE-150's tape reader takes PB7 = 1 as BREAK; before, it saw
>   BREAK all the time.
> - **CE-1600P port 82H reads back its latch.** The tape driver
>   read-modify-writes it; open bus (FFH) would have pulsed both relay coils.
> - **Later replaced** by the cassette bay on the control bar
>   (`TapeManager`, Decisions.md): the File ▸ Tape menu below is
>   gone.
> - No separate `TapeController`: `MainWindow` handles File ▸ Tape (Play…,
>   Record…, Eject), and the control-bar Tape button shows the same actions.
>   The menu items also make the flow scriptable for screenshots.
> - `bin2wav` tapes need `-s 3` or `-l 0x400`: its default leader (0.5 s on
>   the PC-1500, ~2.1 s on the PC-1600) is shorter than the
>   ROM's motor start-up delay plus the leader it counts.
> - Verification: CoreTests (`tape_deck_tests`, `pc1500_tape_tests`,
>   `pc1600_tape_tests`), and `dev/tape-matrix/` (PC-1500A 26/26, PC-1600
>   38/38 against `bin2wav` / `wav2bin`). Real-hardware checks are in
>   TODO.md.


## Context

CLOAD/CSAVE are the biggest remaining gap. Today the ROM tape paths run but
move no data: CE-150 latches its remote bits and ignores them
(`Core/Connector/Ce150Card.hpp:36-38`), CE-1600P keeps only `82H & 0x0F`
(`Core/Connector/CE1600PCard.hpp`), and no machine sources the tape-input
pin. Goal: arm a WAV file (play for CLOAD, record for CSAVE), the user types
CLOAD/CSAVE, the ROM's own tape code runs, and the WAV is interoperable with
`bin2wav`/`wav2bin` (Pocket Tools 2.1.1, `~/Applications/PocketPc/`) and,
ultimately, real hardware.

## Findings that shape the design

**Tape signals live on the main unit, the interfaces only condition them.**
- PC-1600 MODE 0 (CE-1600P bank 5 cassette driver, `pc1600/disasm/rom/ce1600p/new/PC1600-P1-B5-CE1600P-OR-F.asm`):
  pure Z-80 software. Out = OPC port 18H b7 toggled in delay loops
  (`CMTONE0/1` 60EF/60E8, half-periods from F197H..F19AH = 30/23 and 79/71
  loops); in = OPB port 1FH b2, half-cycles timed by `INC B` loops
  (`CMBITIN` 6112, threshold F1A8H). BREAK = port 1BH b1. Motor = CE-1600P
  port 82H b4 (RMT-ON pulse) / b5 (RMT-OFF pulse), b7 = CMT-in enable.
  Format: 1 cycle/bit, "0" ≈3000 Hz, "1" ≈1200 Hz, start "1" + 8 bits MSB
  first, popcount checksum — matches SharpWavAnalysis `spec.md` / bin2wav.
- PC-1600 MODE 1 via CE-1600P: CLOAD reads PC-1500 tapes (`CM1500BIT` 62BD,
  uses divider reset 14H, CL1 on 1AH b7, F=01H); CSAVE is ERROR 110 (ROM).
- PC-1500 + CE-150: LH5811 hardware. Out = serial TX register F006 + F
  register 63H modulator (SDO: 2539 Hz "1" / 1270 Hz "0"), waits on IF b3
  (TD); in = PB2 (`LOAD_NIBBLE` &BE02) plus divider reset F004 and CL1 (MSK
  b7). Motor = CE-150 LH5810 PA1-4 (`REMOTEON` &BF11 / `REMOTEOFF` &BF43).
  None of serial TX / CL1 is modelled today (`PC1500Memory.cpp` `m_ioScratchRegs`).
- PC-1600 MODE 1 + CE-150: open research. (Written as "LH5803 ME1 F00x is a
  plain latch"; since 8858e75 ME1 F000–F00F are the SC7852's ports 10H–1FH.)
  Still undocumented: how SD0 (pin 76) and PB2 reach the CE-150's jack; port
  16H (serial TX) and 1AH b7 aren't modelled on the PC-1600 (TODO.md).
- PC-1600 SDO modulator (F register 17H) already exists, idle-mark only
  (`PC1600Memory.cpp:67-90`); 18H b7 already latched (`m_opc`).

**Full speed is safe.** Every ROM path times the tape in CPU cycles
(software loops or the LH5811 divider, itself clocked in emulated cycles).
If the tape position is a function of *emulated* cycles — never host time —
the WAV is bit-exact at real time, turbo, or in the flat-out CLI. Only the
listening experience needs real time. Same principle already used by
`PiezoSampler` and the CLIs' `--wav` (exact while running flat out).

## Design

### Core: `Core/Tape/`
- `WavFile` (extend `Core/Audio/WavFile.hpp`): add `readWav()` — PCM
  8/16-bit, any rate, mono or stereo (mix down). Keep `writeWavMono16`.
- `TapeDeck` (host-agnostic, no Qt):
  - State: `Empty`, `Play(samples, rate)`, `Record(path)`; `position()`,
    `motorOn()`, `eject()` (finalizes a recording).
  - `setMotor(bool)` — tape moves only while the remote motor runs (real
    CE-152 behaviour; motor-off time is not recorded, playback pauses).
  - `advance(cycles, clockHz)` — moves the tape in emulated time.
  - Play: DC-blocked sample → Schmitt comparator with hysteresis → `inputLevel()`
    (models the CMT input amplifier; handles 8-bit bin2wav, real recordings,
    either polarity).
  - Record: `setOutputLevel(bool)` box-filtered into 48 kHz 16-bit (reuse the
    `PiezoSampler` box filter, Transducer::None, no DC blocker needed — or
    plain square); flushed with `writeWavMono16` on eject.
- Bus wiring respects "cards know only the bus": the main unit drives bus
  CMTOUT (pin 29) and reads CMTIN (pin 27) (`Core/Connector/SystemBus.hpp`
  already names them); `Ce150Card` / `CE1600PCard` own the jack + remote
  relay and connect the deck to those pins. Deck pointer lives on the card;
  machines expose `tapeDeck()` for the controller.

### Per-machine signal work
1. **PC-1600 MODE 0 (smallest gap):** `PC1600Memory` — OPC b7 → CMTOUT,
   CMTIN → PB2 in `m_pbIn` (1FH read); `CE1600PCard` — 82H b4/b5 latching
   relay → `setMotor`, b7 CMT-in enable gates input; deck `advance()` from
   `PC1600Machine::advanceSharedClocks`.
2. **PC-1500 + CE-150:** shared LH5811 serial block (TX shift register,
   TD flag, F-register modulator SDO, divider reset, CL1) — extract from
   `PC1600Memory`'s modulator into `Core/CPU/LH5811Serial.{hpp,cpp}`, used by
   `PC1500Memory` (F004/F006/F007/F009/F00A/F00B) and `PC1600Memory`
   (14H/16H/17H/1AH). CL1 source is **[open]** — research step first
   (TRM p.69-72, CE-150 `LOAD_NIBBLE`), no signal modelling until settled.
   `Ce150Card` LH5810 PA1-4 → remote relay → `setMotor`; call
   `Ce150Card::tick` already hooked at `PC1500Machine.cpp:171`.
3. **PC-1600 MODE 1 CLOAD via CE-1600P** — falls out of 1+2 (needs CL1).
4. **PC-1600 MODE 1 + CE-150** — deferred: research note in TODO.md
   (LH5803 F00x ↔ SC7852 SD0'/PB2), record in Decisions.md if blocked.

### CLI (test + automation backbone)
`tools/pc1500_cli.cpp`, `tools/pc1600_cli.cpp`: `--tape-in file.wav`
(armed for play) and `--tape-out file.wav` (record, finalized at exit),
next to the existing `--wav`. Combined with existing key typing this gives
fully scripted CSAVE/CLOAD.

### GUI (Qt6)
Control-bar "Tape" row (pattern: floppy row in `ControlBar` +
`FloppyDiskManager`), shown when CE-150 or CE-1600P is attached:
- **Play…** (pick WAV → armed for CLOAD), **Record…** (pick save path →
  armed for CSAVE), **Stop/Eject** (finalizes recording), motor lamp
  (`setFloppyMotorOn` pattern), tape counter (mm:ss), arm state text.
- Speed: real time, no separate toggle. The existing press-and-hold-LCD
  turbo works as usual; audio is suppressed there as today, but the deck
  keeps running in emulated time, so the recording/playback is unaffected.
  The ROM-driven test runs the deck once at real-time pacing and once
  flat out to prove the files are identical.
- Real time: PC-1600 CSAVE is audible through the existing buzzer path
  (OPC b7 is also the speaker line). Optional "monitor" of playback input
  is out of scope.
- New `TapeController` in `Qt6/app` owning arm state + remembered folder
  (`AppSettings::openStartDir` pattern). Not persisted unless asked.

## Testing strategy

1. **Unit (CoreTests):** WAV read/write round trip (8/16-bit, 44.1/48 kHz,
   stereo); comparator on synthetic FSK incl. inverted polarity and noise;
   motor gating; record box filter.
2. **ROM-driven (CoreTests, skip without ROMs), no external tools:**
   self round trip per machine — type a program, CSAVE into a deck,
   `NEW`, CLOAD from the recording, compare program memory; `CLOAD?`
   verify; checksum error on a corrupted sample.
3. **Interop fixtures:** small `bin2wav` WAVs checked in under
   `Core/tests/data/tape/` (one PC-1500 BASIC, one PC-1600 BASIC, one
   PC-1600 BIN) → CLOAD in tests; (PC-1500 tools user-verified).
4. **One-time matrix in `dev/tape-matrix/`** (own build, per memory rule):
   for N programs × {PC-1500, PC-1600 MODE 0, PC-1600 MODE 1 CLOAD}:
   `bas2img`→`bin2wav`→CLI `--tape-in`→CLOAD→compare, and
   CLI CSAVE `--tape-out`→`wav2bin`→compare with `bas2img`. Also run once
   with emulated time stretched (turbo-equivalent) to prove speed
   independence. Where PC-1600 tools disagree with the ROM, the ROM wins
   and the deviation is documented.
   `bin2wav`/`wav2bin` are the testbed of record (user: accurate enough).
5. **Real hardware (user, final check, later):** PC-1600 + CE-1600P and
   PC-1500 + CE-150 are available — play an emulator CSAVE WAV into the
   real unit's CLOAD, record a real CSAVE and CLOAD it in the emulator.

## Phases (each: implement, tests green, commit to dev-0.8.0)
1. WAV reader + `TapeDeck` + unit tests.
2. PC-1600 MODE 0 wiring + CLI flags + ROM round-trip test + bin2wav fixtures.
3. dev/tape-matrix for PC-1600 MODE 0 (answers "are the PC-1600 tools right").
4. Research CL1/serial TX (doc note), then `LH5811Serial`, PC-1500 + CE-150,
   PC-1600 MODE 1 CLOAD; extend tests + matrix.
5. GUI tape row (real time, usual LCD turbo); user guide chapter + screenshot scenario.
6. Docs: plan → `docs/background/plans/Cassette-Tape-Plan.md`, README row,
   Decisions.md entries (deck moves only with motor, ROM-wins rule), TODO
   for MODE 1 + CE-150.

## Verification
- `tools/run_tests.sh` green after each phase.
- `dev/tape-matrix/run.sh` all rows pass (both directions).
- GUI: arm Play with a `bin2wav` file, type `CLOAD`, watch the motor lamp
  and counter, `LIST` shows the program (check via `--lcd-text`/DAP screen);
  arm Record, `CSAVE "X"`, Stop, `wav2bin` decodes it. Launch with
  `-ApplePersistenceIgnoreState YES`, quit via calcu1600/quit.
