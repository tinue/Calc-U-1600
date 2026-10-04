# TODO

The living backlog: known bugs, unbuilt features, and pre-release
obligations.

## Known issues

- **PC-1600 BASIC runs ~0.4-0.65% fast vs a real unit (MODE 0); cause open.**
  Measured 2026-09-23 from audio recordings (BEEP markers, ms precision;
  the recorder reads 0.22% slow, calibrated from the F-register whistle).
  Every BASIC program is ~0.65% short:

  | Program | Real | Emulator (dev-0.5.0) | Emulator (2026-09-26) |
  |---|---|---|---|
  | A: `FOR I=1 TO 2000:NEXT I` | 6.275 s | 6.235 s (−0.65%) | 6.251 s (−0.38%) |
  | B: 100 scrolling `PRINT`s | 5.894 s | 5.854 s (−0.68%) | 5.868 s (−0.44%) |
  | C: A with sub-CPU interrupt masked (`OUT 53,&1F`) | 6.230 s | 6.189 s (−0.65%) | 6.191 s (−0.63%) |
  | D: A with all interrupts masked (`OUT 53,0`) | 6.011 s | 5.974 s (−0.62%) | 5.974 s (−0.62%) |
  | dampflok.bas, whistle 1→9 | 65.281 s | 64.843 s (−0.67%) | not re-run |

  C and D still carry ~70 T (~19 µs) per `FOR/NEXT` pass, i.e. a fixed cost
  per statement/pass rather than a percentage, with no sub-CPU involvement.
  With the sub-CPU's 0.5 s event reported on every tick (below), A−C is
  60 ms against the real 45 ms: the fitted sub-CPU response time is too
  long (see the timing-model item).

  **Found and fixed on the way** (dev-0.5.0):
  - One wait per M1 cycle (f05e42e): BEEP pitch at two A values.
  - Sub-CPU IOCS 25H fast-path probe (6d6318a): the BEEP repeat slips.
  - Sub-CPU response time of 1.66 ms per command byte (64538ae): A−C.
  - LCD busy until the 4th LCD-clock edge (c18411b): B.
  - The ON key's live PB7 level (5011e2b).
  - SRIRQ reports the 0.5 s event on every tick (dev-0.6.0, sub-CPU
    rework), so every INT6 handler runs the 0.5 s housekeeping (battery
    check, SRINP, APO countdown): A and B +15 ms.

  **Ruled out**, all with interrupts off, emulator matching real within
  ±0.1–0.2%. Test programs are in `headless/beep/bench/timing{,2,3,4,5}.bas`
  (gitignored); `presetprobe` computes the emulator side:
  - Opcode fetches from RAM and from every ROM bank the interpreter uses
    (page A, page B banks 0/3, page C bank 6).
  - Data and operand reads from all those ROM banks; RAM reads and writes
    at C000/DFxx/F5xx/FAxx.
  - `IN` from 18H/31H/59H; `OUT` to 31H/3DH (bank switching).
  - `EX (SP),HL`, CALL/RET, PUSH/POP; IX-indexed, `CB`, `ED`, `DD CB`
    instructions; `LDIR`, `RLD`/`RRD`, `DAA`, `ADC`/`SBC`, 16-bit adds,
    `EX`/`EXX`, `LD (nn)`, `INC`/`DEC (HL)`, ALU ops, JP/JR/RET cc.
  - An extra or unmodelled interrupt source: a NOP loop with EI and mask
    00H matches (so the mask gates everything on hardware; D has no
    interrupts at all yet is still slow), and with the normal mask it
    matches too (the LH-5803 cause bit and all sub-CPU events enabled).
    The 64 Hz and 0.5 s handler costs match (real A−D 264 ms vs 261 ms).
  - How the program got into memory (typed vs binary-loaded: identical).
  - The other bundled ROM set: "old" is a further 0.65% faster.

  **Ruled out by the ROM (2026-09-27): work-area state the emulator never
  produces.** The idea was that the real unit took a longer per-statement
  path because of F127H/F88DH. The disassembly settles it:
  - The statement loop (P0-B0 3AC0H) reads F127H only for b5 (`ALARM$`).
    b6 (`ON TIME$`) is set by the default hook and cleared only by a
    dispatched `ON TIME$ GOSUB` (3D40H) or a new timer write, so F127H=64
    is a leftover flag with no per-statement cost.
  - The BASIC-interrupt check costs extra only while F1CFH/F1D0H ≠ 0, and
    `RUN` clears F1CFH–F1D4H (`RUNSET`, 1CA2H–1CA8H).
  - F88DH is the TRON flag (0 = TROFF). F88EH=2 is TRONMODE, which `RUN`
    writes every time (P1-B0 5803H).

  So benchmark A takes the same path on both. Re-timing it on the real unit
  with `ON TIME$` cleared would show nothing. Next: copy the FOR/NEXT
  arithmetic routine into RAM and time it there.

  MODE 1 (LH-5803) hasn't been re-measured since the host-pacing fix; the
  old "~7% fast" figure predates it.
- **Parameterless `- wait:` in a preset can inflate a program's own
  `TIME` measurement** (~2.3x observed) in the GUI, while `- wait: <n>`
  and no wait agree with each other and with headless runs. Working
  hypothesis: while a BASIC program runs, the LH5803 owns the bus and the
  SC7852's 64 Hz/0.5 s timer accumulators are frozen, so a pure headless
  `FOR/NEXT` sees no timer-ISR overhead; the GUI's parameterless-`wait:`
  handback path appears to let the SC7852 step during the loop, so the
  timer ISR runs every iteration and `TIME` reports the (arguably more
  realistic) larger cost. If so, this is a "timers freeze under LH5803
  ownership" gap, not a `wait:`-specific bug.
  **What the ROM says (2026-09-27):** BASIC runs on the Z-80 in both MODEs.
  The LH5803 gets the bus only through `CALLH`, but that happens often:
  every relational operator (`CMPNUM`/`CMPSTR`, P1-B3 5D3FH), `^`,
  `AND`/`OR` and the functions. An integer `FOR/NEXT` never calls it (Z-80
  add, inline compare at P1-B0 5AA1H). So the premise holds only for
  programs that compare or call functions in their loop, and then only
  for the length of each call. Check which program showed the 2.3x, and
  what the emulator does differently on the headless and `- wait: <n>`
  paths.
- **Open bus reads as a constant FFH; real hardware returns the last byte
  on the data bus. To decide.** PC-1600 MODE 0, CE-163F in Slot 2:
  `XPEEK&C5` returns 37 on a real unit and 255 in the emulator. In MODE 0
  the Z-80 hands over with Port 31H = 06H (page C = bank 0, the empty
  Slot 1) and `P_MAPPRG` leaves it there, so the mapping is right. The
  LH5803's PEEK reads with `lda (u)` (rom1500 D997), opcode 25H = 37: the
  floating bus still holds the opcode just fetched. Decide whether to
  model this (last data-bus byte per CPU/bus, on both machines?) or keep
  FFH and record the choice in Decisions.md. A change would reach every
  open-bus read, e.g. the RAM-sizing probes and the empty-slot checks in
  the tests.

## PC-1600 serial port

- **RS-232C / SIO connector mux.** PRIME (the PRIM select) is tracked in
  `TC8576F::rs232Selected()`; both connectors still share the one
  `SerialLink`, whose file is `calcu1600-rs232c.serial`. The split gives
  SIO its own `calcu1600-sio.serial` (Decisions.md, "Serial port files are
  named after their connector"), carrying data only while PRIME selects
  SIO; `calcu1600-rs232c.serial` then carries RS-232C only.
- Capture the exact on-wire `SAVE"COM1:"`/`LOAD"COM1:"` framing from a
  real ROM trace, and do an end-to-end round-trip against real
  SharpDataExchange.
- Follow-on: a localhost-socket transport (`SocketSerialLink`) so
  `OUTSTAT 0-3` and buffer-full RTS become effective end-to-end, for
  serial-only tooling to bridge via `socat`.

## PC-1600 loading: still open

The loaders follow MODE and `TITLE` (docs/background/plans/Loader-Mode-Plan.md, done). Left:

- **The open questions of the load/save matrix**
  ([Ref/PC-1600/PC-1600-Load-Save-Matrix.md](https://github.com/tinue/Sharp1500-1600-Ref/blob/main/PC-1600/PC-1600-Load-Save-Matrix.md) §6), to be discussed: are all tokens
  the PC-1500 and PC-1600 share identical (the MODE 1 listing rule assumes
  so; a table comparison of libsharpdx's two tables against the ROM's would
  settle it); `INPUT#-1` in MODE 1 through the CE-1600P (the ROM allows it,
  the TRM doesn't); `SAVE`/`LOAD "CAS:"` in MODE 1; whether the CE-158's own
  `SETDEV` is reachable on the PC-1600; CE-150/CE-158 `PRINT#`/`INPUT#` in
  MODE 0.
- **MODE 1 + CE-155: the fast loader writes the program to the wrong
  place.** With a CE-155 in slot 1 and `MODE1` + `NEW0`, the ROM's area
  starts at LH5803 &20C5, and typed lines land at CE-155 backing offset
  0x08C5; `loadBasicProgram` puts them at 0x00C5 (it takes the slot window
  &A000 as backing offset 0). The work-area pointers match the typed ones,
  so LIST looks right, but RUN doesn't run the program. Found when the
  "MODE 1, CE-155" case of `test_work_area_matches_typed`
  (`Core/tests/pc1600_basicloader_tests.cpp`) was made to run: it had
  silently returned early since it was written (an 8 KB `plainRamCard` is
  not a valid definition). The case is commented out there until this is
  fixed. **Before fixing:** check how `PC1600ProgramPlacement` maps a
  module segment to the card's backing store -- the CE-155's 2K/6K chip
  split in the PC-1500 map is the likely mismatch -- and whether MODE 0
  with a CE-155 has the same problem.
- **Guide screenshots write into the real saves folder:** the chapter-5
  preset (`docs/developer/screenshots/presets/pc1600-modules.pc1600`) uses
  `saveas: live slot-1:My programs`, so every `tools/make_screenshots.sh`
  run writes a live "My programs" card into the Battery-card saves folder.
- **`examples/memory/flashtest_ce163f.pc1500a` stops with ERROR 1 IN 10.**
  The lines are stored, but with
  the CE-163F in the slot, BASIC's program area starts at &00C5 inside the
  banked window, and line 10 (`POKE &6809,0`) switches that bank away
  from under the running program. The startup presets move BASIC up with
  `NEW&112`, which doesn't help here: the program pokes into the bank it
  lives in. **Before fixing:** decide whether the demo becomes direct-mode
  `type:` steps (as in its header comment) or a program placed outside
  &0000-&3FFF.

## Expansion connectors: one model on both machines

**Goal:** the connectors are fundamentally the same on the PC-1500 and the
PC-1600, so software models them the same way. Each physical connector is
one connector object. The host drives the signals, and a card sees only
those signals, never the host (docs/background/Decisions.md, "Cards know only the
bus"). The 60-pin connector extends the 40-pin one. They stay two plugs,
but share one signal vocabulary and one connector/chain shell.

**Requirement: every peripheral eventually goes through the emulated
60-pin connector, with no shortcuts.** This covers the CE-150, CE-158,
CE-1600P and CE-1600F, and anything added later. A card gets its signals
only from the connector's contacts, numbered as the real 60-pin plug
numbers them. The host never builds a `PinState` by hand, never holds a
typed card pointer, and never decodes a card's address ranges. **The
connector must be modelled properly on the PC-1600 too.** That means one
60-pin connector object, driven by whichever CPU owns the bus (the LH5803
while ELH is low, otherwise the SC7852 through its gate array), carrying
what the real SC7852 puts on each contact (PVOUT, PU, PT, IORQ/IOE, ...).
It is not a copy of the PC-1500's signals.

**Shortcuts in place today** (each one must go):
- 60-pin PU/PV sit on `pin[3]`/`pin[2]`, their **40-pin** contact
  numbers (`PC1500SignalDecode::basePinState`). The CE-150 and CE-158
  read them from there. Measured 2026-10-03: 60-pin 15 = PU, 16 = PV on
  both machines, and the PC-1500's 40-pin pins are 2 = PU, 3 = PV, not
  the TRM's 2 = PV, 3 = PU (see below).
- On the PC-1600, `LH5803SharedMemory::peripheralPins` hands the
  LH5803's own PU and PV flip-flops straight to the cards. On real
  hardware PV goes out through the SC7852 as PVOUT. The LH5803's PU has
  no documented path to the connector, because the SC7852's PU output is
  a Port 31H bit.
- The rest of the list follows below: `PC1600BusPins` and two separate
  60-pin paths on the PC-1600. (Typed `Ce150Card*`/`Ce158Card*`,
  `isCe158Io` and the PC-1500's `kCe150IoBase` are gone, see below.)

**Hardware facts** ([Ref/Shared/Expansion-Connectors.md](https://github.com/tinue/Sharp1500-1600-Ref/blob/main/Shared/Expansion-Connectors.md) §2, §4):
- Both connectors carry the address bus, data bus, PU/PV, INHIBIT, DME0,
  R/W and OD.
- The 60-pin one adds ME1/DME1, INT, WAIT (WEX/W1), CMTIN/CMTOUT, VBAT, BFO
  and φOS.
- The PC-1500's 40-pin connector also carries decoded chip selects
  (Y0, Y2, S1–S4) that aren't on the 60-pin one, so neither signal set
  strictly contains the other. The PC-1600's 40-pin slots carry
  RAM1/RAM2, PVOUT, PT, K0–K2/S1–S3 and MREQ instead.
- The PC-1600's 60-pin connector matches the PC-1500's on every common
  signal and pin. The CPU-specific pins differ: PT/PU/PVOUT, RD/WR, IORQ,
  MREQ, M1, ELH, IOE. That's what the host drives, not what the card
  sees.

**Done 2026-09-26 (mechanical groundwork, docs/background/plans/Expansion-Connectors-Plan.md;
the pins reaching the cards are unchanged):**
- One card-chain shell, `CardChain` (attach/detach, InhibitSource cache,
  first-responder read/write), behind all four connector classes. The
  40-pin plugs are chains of one. The debug/name virtuals moved to
  `CardBase` so a later 60-pin card interface can share them.
- `PC1500Memory` owns its `ExpansionConnector` and `SystemBus` by value;
  the raw-pointer setters are gone.
- The hosts don't know which card sits where. `LH5803SharedMemory`
  offers its peripheral accesses to one chain,
  `PC1600Memory::lh5803PeripheralBus()`, instead of typed pointers and
  `isCe158Io`. `PC1500Memory` gives the 60-pin bus first refusal on every
  ME1 access instead of hard-coding the CE-150's LH5810 window. Debug
  peeks ask `ExpansionCard::readHasSideEffects()`.
- The PC-1600's two 60-pin halves (`lh5803PeripheralBus()` and
  `ce1600pBus()`) now sit side by side in `PC1600Memory`, ready to become
  one object once the signals are settled.

**Paths before the groundwork above (five, not two):**

| Connector | Code | Card interface |
|---|---|---|
| PC-1500 40-pin | `ExpansionConnector` | `ExpansionCard` / `PinState` |
| PC-1600 40-pin slots | `MemorySlotConnector` | `ExpansionCard` / `PinState` ✓ |
| PC-1500 60-pin | `SystemBus` | `ExpansionCard` / `PinState` |
| PC-1600 60-pin, LH5803 side (CE-150, CE-158) | `LH5803SharedMemory::peripheralPins` / `cardRead` | `PinState` built by hand |
| PC-1600 60-pin, SC7852 side (CE-1600P, CE-1600F) | `PC1600SystemBus` | its own `PC1600ExpansionCard` / `PC1600BusPins` |

What's wrong with that:
- **`PinState` numbers signals by 40-pin contact.** `SystemBus` reuses
  it for the 60-pin connector and sets Y0/Y2 and S-block pins (via
  `PC1500SignalDecode::basePinState` and `sBlockPin`) that the 60-pin
  connector doesn't carry. On the 60-pin connector, contacts 16–18 are
  PU/D7/D6. It's harmless today because the CE-150 and CE-158 read only
  address, ME1 and PV/PU, but the model is wrong.
- ~~**The PC-1600 has no 60-pin connector object on the LH5803 side.**
  `LH5803SharedMemory` holds typed `Ce150Card*`/`Ce158Card*` pointers
  with a fixed order and knows the CE-158's address ranges (`isCe158Io`).
  So the host knows the card.~~ Done: a card chain, but still separate
  from the SC7852 side and still fed hand-built `PinState`s.
- **`PC1600BusPins` isn't a pin model.** It's a ROM offset plus a
  `bank5` flag. On real hardware, bank 4/5 at 4000–7FFF reaches the
  CE-1600P via the Port 31H page-B field on the PT/PU/PVOUT pins.
- Because of this split, the same physical 60-pin connector exists twice
  on the PC-1600. That's why the CE-158 and the CE-1600P can't be
  attached together (see Feature ideas).
- ~~`MemorySlotConnector` and `ExpansionConnector` share ~25 lines of
  copy-pasted dispatch. `PC1500Memory` gets its `ExpansionConnector` and
  `SystemBus` by raw pointer from `PC1500Machine`, while `PC1600Memory`
  owns its connectors by value.~~ Done.

**Direction** (to confirm in the analysis below):
- Keep the physical-contact principle (docs/Memory-Card-Definition-Spec.md:
  "the loader operates on physical pin numbers, full stop"), but number
  each plug by its own real contacts. A 40-pin card sees 40-pin contacts.
  A 60-pin card sees 60-pin contacts (e.g. PV 15, PU 16, ME1 59) instead
  of today's 40-pin numbers plus a `me1` flag. Each connector class maps
  host state onto the contacts it physically has, and the signals both
  plugs share get one piece of mapping code.
- One connector/chain shell (attach/detach, INHIBIT, read/write) for both
  plug types. A 40-pin slot is a chain of one.
- One 60-pin connector object per machine. On the PC-1600, both CPUs drive
  it: the LH5803 side when it owns the bus (ELH), and the SC7852 side. The
  CE-150, CE-158, CE-1600P and CE-1600F all become `ExpansionCard`s on it.

**Before fixing:**
- Research the PC-1600 60-pin connector signals that the cards need.
  **Mostly settled 2026-09-26** from the Service Manual's SC7852 pin table
  (printed pp. 20–21) and memory map (§5-3):
  - When the LH5803 owns the bus, the SC7852 pins turn into inputs that
    carry the LH5803's signals: IORQ = ME1, MREQ = ME0, RD = OD,
    WR = R/W, M1/RFSH made from OPF. So on the connector, IORQ (26) *is*
    LH5803 ME1 while ELH (58) is low.
  - IOE (59) is not raw ME1. It is a decoded strobe the SC7852 raises
    only for LH5803 ME1 accesses to `xx00–xx0F` and `8000–FFFF`, with
    one wait (LHWAIT), half a clock after ME1. It never fires for the
    Z-80.
  - ELH (58) low = LH5803 running: the ownership signal.
  - PV: "the PV signal of the LH-5803 is directly sent by PVOUT"; ME1
    `8000–BFFF` is the CE-150 at PVOUT = 0, the CE-158 at PVOUT = 1.
  - **Settled 2026-10-03 by measurement: 60-pin contact 15 = PU, 16 = PV
    on both machines** (PVOUT on the PC-1600, which carries the LH5803's
    PV). On the PC-1500, LH5801 pin 60 (PV) beeps to contact 16 and pin 61
    (PU) to 15, and the **40-pin connector has pin 2 = PU, pin 3 = PV**.
    Both PC-1500 TRM tables have PU/PV swapped. Contact 44 is F-GND, not
    VBAT. Details in [Ref/Shared/Expansion-Connectors.md](https://github.com/tinue/Sharp1500-1600-Ref/blob/main/Shared/Expansion-Connectors.md) §2.2b.
  - PC-1600 nets (SM schematic): LH5803 PU shares one line with the
    SC7852's PU output (slot pin 3, 60-pin 15); LH5803 PV goes SC7852
    PVIN → PVOUT (slot pin 5, 60-pin 16). Slot pin 2 ("PVIN" in the TRM)
    is VCC (the CE-1620M's EPROM Vpp); the model leaves `pin[2]` low there.
  - **Model numbering is off on the PC-1500:** hosts and cards agree
    on `pin[3]` = PU and `pin[2]` = PV, so behaviour is right, but the
    real 40-pin contacts are 2 = PU and 3 = PV (`SystemBus`,
    `PC1500SignalDecode::basePinState`, `resolveSignalPin`, the CE-150/
    CE-158/bus-ROM cards, `LH5803SharedMemory::peripheralPins`). The
    PC-1600 slot's `pin[3]` = PU is correct.
  - Not yet looked at: which pins tell bank 4 from bank 5 for the
    CE-1600P ROM, and how its I/O ports show up (the CE-1600P PDF).
- ~~Why `PC1500Memory` gets its `ExpansionConnector` and `SystemBus` by
  raw pointer.~~ Settled 2026-09-26: no reason survives. The setters date
  from the squashed v0.1.0 import. `PC1500Machine` wires them right after
  constructing `m_memory` (declared before the connectors, which is why
  they can't be constructor arguments today). The bare `PC1500Memory`
  instances in `lh5801_tests.cpp` never attach a card, so empty owned
  connectors behave the same as the null pointers. Converge on
  `PC1600Memory`'s pattern (owned by value, `ExpansionConnector(variant)`
  / `SystemBus(variant)` built from the memory's own variant), with the
  machine's `expansionConnector()`/`systemBus()` forwarding. On the
  PC-1600, the single 60-pin connector has to be reachable from both
  `PC1600Memory` and `LH5803SharedMemory`. Done for the PC-1500. On the
  PC-1600 both halves now live in `PC1600Memory`, which
  `LH5803SharedMemory` already reaches (as it does for `uart()`), so the
  merged object can stay there with no machine-level wiring.
- `.card.yaml` files and `SoftwareDefinedCard` use 40-pin contact numbers,
  and that stays. Check that nothing in the 40-pin path changes when the
  60-pin side moves to its own contact numbering (the memory-card tests
  should pass unchanged).

## Cassette tape: still open

CLOAD/CSAVE work on the PC-1500/1500A + CE-150 and the PC-1600 (MODE 0) +
CE-1600P (docs/background/plans/Cassette-Tape-Plan.md, dev/tape-matrix/).

- **PC-1600 MODE 1: `CLOAD` of PC-1500 tapes through the CE-1600P.** The
  reader (`CM1500BIT`, bank 5 62BDH) writes F (17H) = 01H, resets the
  divider (14H) on each input edge and times the bit against MSK 1AH b7.
  On the PC-1500 that bit is CL1, wired to the G-register serial clock
  (CL0). But no PC-1600 ROM bank writes G (19H), and Baum's Appendix 6
  names 1AH b7 "RD". **Before fixing:** find what drives 1AH b7 on the
  SC-7852. Candidate: the FX clock (F = 01H, phi/128 = 2539 Hz), which
  would make the timer one PC-1500 "1" half cycle. Real-unit check: a short
  machine-code loop doing `OUT (17H),01H`, `OUT (14H),A`, then counting
  `IN A,(1AH)` b7 toggles for a fixed number of loops, with and without
  F = 00H. Only then model it (LH5811Serial already has the divider).
- **CE-150 on a PC-1600 (MODE 1): no tape path.** The CE-150 ROM runs on
  the LH5803 and drives ME1 F004H-F00FH, which `LH5803SharedMemory` keeps
  as a plain latch. How that block reaches the SC-7852's SD0/PB2 (pin 76:
  `SD0 = OR(SD0', PC7')`, SD0' = "CE-150 cassette output") is undocumented.
- **Real-hardware cross-check, remaining half:** `CLOAD` an emulator
  `CSAVE` WAV on the real units (PC-1500 + CE-150, PC-1600 + CE-1600P).
  The other half is done (2026-10-04): real `CSAVE` recordings of both
  machines load in the emulator, and their timing set the recorder's 12 Hz
  coupling and the LH5811 transmitter model (Decisions.md).

## Feature ideas

- **LCD text: the kana set.** No font that the parser reads has katakana
  yet, so they come out unparsed. On the PC-1500 they come from the second
  character set at (KATACHAR) when KATAFLAGS enables it (`CHAR_2_ADDR_4`,
  EE5AH). On the PC-1600 they appear in PC-1500 mode (LH5803 ROM
  `KANA_LCD` C700H for 80H–D8H, `KANA_LCD_D9` C6BDH for D9H–E5H). Copy
  Screen's mapping is ready: `jisX0201Kana()` in
  `Core/Display/LcdCharsets.hpp` (A1H–DFH → U+FF61–FF9F).

- **Sub-CPU F-pin tones: key click, `ALARM$` beep, wake-up beep, hour
  signal** (deferred until measured). The sub-CPU's F output drives the
  buzzer for SBEEP (IOCS 01H, key click with `KEY` click on), the 1 s
  `ALARM$` beep, the wake-up beep (SWPON bit 2) and the hour signal (SWPON
  bit 3). No source gives frequency, length or waveform. Record each on
  the real unit with the calibrated iPhone recorder (as for the BEEP
  measurements): key click; `ALARM$` a minute ahead; `WAKE$(0)` a minute
  ahead after `POWER OFF`; the hour signal if SWPON bit 3 can be set from
  BASIC. Then drive `PiezoSampler` from a modelled F output
  (`PC1600SubCpu`, `PC1600Memory::updateBuzzerLine()`).
- **Sub-CPU commands still unnamed** (IOCS 0CH–0FH, 1BH, 1FH, 26H, what
  1CH/1DH mean, the LH-5803's DCH): see [Ref/PC-1600/PC-1600-SubCPU-LU57813P.md](https://github.com/tinue/Sharp1500-1600-Ref/blob/main/PC-1600/PC-1600-SubCPU-LU57813P.md) §8.
  The ROM trace is done (2026-09-27): 1EH is the port-mode select
  (F12CH b0 analog input, b1 external keyboard; `ON ADIN`, `KEYSTAT`),
  16H/17H are the external keyboard, 1CH/1DH come from `SINIT`. The rest have
  no caller in the system ROMs, so only real-unit tests can name them.

- CE-158 together with the CE-1600P. The real CE-1600P has its own
  connector at the back (like the CE-150), so both can be attached at
  once; today `PC1600Machine::attachCE1600P`/`attachCE158` detach each
  other and the preset parser rejects the pair (blocked on "Expansion
  connectors: one model on both machines"; User Guide says "not yet
  supported").
- **Review the Debug panel's content against the full ROM disassembly.** The
  pointer dump and the other views were built before the ROM was fully
  commented. Go through the disassembly's work-area symbols and pick what
  helps when debugging: e.g. `TITLE`, the S1/S2 slot tables, `ADTBL`,
  `BMODE`, `BINTREQ`/`F1CF`–`F1D4`, TRON state, `OPNDV`, the logical banks
  F1C1–F1CE, PRGADR FE3C–FE41. Also check every existing label and note for
  accuracy (address space, byte order, meaning).
- Allow saving a diskette or memory module into a preset after it has been
  set up (e.g. formatted / populated in a session), so the preset carries
  that media state.
- Allow viewing a memory module's or diskette's binary file contents in the
  debug area, without having to save them out first. For memory modules
  this means the disk part (the RAM-disk filesystem), not the RAM-extension
  part. For a floppy side, the vendored libsharpdx (0.3.0) already has
  `sde_disk_list` / `sde_disk_get`, which decode the directory and files
  (BASIC as a listing) from the in-memory image; nothing calls them yet.
- Watch an inserted floppy's `.floppy.yaml` for outside changes (e.g.
  `sde put` while the disk is in the drive) and reload or warn, instead
  of overwriting them at the next autosave. Until then the rule is
  "eject first" (`docs/developer/Floppy-Image-Format.md` §8).

## Code cleanup backlog

Refactors and internal costs, not user-facing bugs. Take them when the
area is next touched; entries marked *(behaviour/timing)* change what the
emulator does and need a deliberate check. Entries with a **Before
fixing** step need that analysis first. A fix that only moves the cost
somewhere else doesn't count (see docs/background/plans/Code-Cleanup-Plan.md).

- **Picker and serial-status plumbing is written twice.** Independent of
  the connector model; can be done any time.
  - `CE1600PRomVersion` clones `PC1600RomVersion`, and
    `BundledRoms::isCE1600PRomVersion` == `isPC1600RomVersion`.
  - The enum↔string mapping is inlined in `MachineController.cpp`
    (`versionName` lambda, `loadPC1600RomSet`) and `PresetController.cpp`.
  - The old-ROM fallback `QMessageBox` is copied.
  - MainWindow's ROM menu builder/sync and ControlBar's combo are
    copy-pasted, and the four `apply*Selection` handlers each carry an
    "already checked" guard (dff5d13).
  - `MachineController::serialLinkStatus`/`ce158SerialLinkStatus` are a
    pair.

  Fix: one `NewOldRom` enum with to/from-string, one
  `warnOldRomFallback()`, one menu/combo builder parameterised by label and
  slot, and one static status helper over a `PtySerialLink*`.

  **Design constraint:** the four "already checked" guards must end up as
  *one* guard inside the shared picker (call onPick only when the value
  changes). Replacing them with `toggled(true)` plus `QSignalBlocker`s in
  every sync setter only moves them.
- **Peripheral buttons are one setter per peripheral.**
  `ControlBar::setCe150State`/`setCe158State`/`setCe1600pState`,
  `PlotterController`'s per-peripheral toggles and
  `MachineController::attachCE150/158/1600P` + `detach*` repeat the same
  shape, and `MainWindow::syncPeripherals()` hard-codes the exclusion rules
  ("CE-1600P excludes CE-150 and CE-158").

  **Order: do this after "Expansion connectors: one model on both
  machines".** The exclusions come from today's split 60-pin model (the
  same plug exists twice on the PC-1600), and the connector work changes
  both the rules (CE-158 + CE-1600P become possible) and the attach API
  (cards on one 60-pin chain). Unifying the buttons first would bake the
  current pairwise rules into the shared setter. Target afterwards: one
  button setter and one attach/detach path per peripheral kind, with
  "enabled" derived from what the connector chain can take instead of
  hard-coded pairs.
- **Card/floppy template-vs-instance rules are written twice and re-parse
  files.** `MemoryModuleManager` (`moduleLists`, `classifySlot`,
  `userTemplateNames`, `saveSlotAs`) and `FloppyDiskManager` (`diskLists`,
  `classifySource`, `saveDiskAs`) each hold bundled-first shadowing, the
  template/instance split, the Name & Save checks and the "never
  autosave under the bundle" rule. They also re-read files Core just
  parsed: `classifySlot` fully parses the `.card.yaml` again (MB of
  `initial-content` hex for superRAM 512K) on every rebuild and twice per
  preset load; `saveSlotAs` parses the instance dir 3x; and
  `refreshModuleCombos` scans + parses both dirs once per slot. The name
  lookup itself is the biggest cost: `resolveModuleSpecByName` scans and
  *fully* parses every `.card.yaml` in both dirs to find one name. So
  attaching one module parses every card file, then the chosen one twice
  more (`makeSoftwareDefinedCard`, `classifySlot` via
  `readMemoryCardCatalogEntry`, which is a full parse too).

  **Dependencies (checked 2026-09-26):** none on the expansion-connector
  chapter. This is file-catalogue logic, `.card.yaml` keeps 40-pin contact
  numbers, and `compatible-hosts` is only a load-time gate (the built card
  keeps no host). The planned `{path, isTemplate, battery}` result matches
  the reserved `batteryBacked` flag (docs/background/Decisions.md). **Do this before**
  the feature ideas that build on this layer: saving a diskette/module
  into a preset, viewing their contents, and watching `.floppy.yaml` for
  outside changes.

  **Parser analysis (done 2026-09-26).** `isRom()` needs only each
  region's content kind, `by-bank` ranges and bank count, which
  `parseRegion()` has before it reaches `initial-content`. Measured on a
  synthetic superRAM 512K instance (`headless/cardparse/`): 2.2 MB of
  random content costs 24 ms to parse fully, of which `parseYaml` is 8 ms
  and the hex decode 16 ms. A sparse one (0.5 MB) costs 8 ms (4 + 4). The
  YAML before `initial-content` takes 0.03 ms. Every card file today is
  single-region with `initial-content` last, but the format doesn't
  require that, so a text cut like the floppy's `\nsides:` isn't safe for
  cards. Target: a catalogue parse mode that skips `parseInitialContent()`
  and the ROM coverage check (≈3x cheaper). Only if that isn't enough,
  let the YAML reader skip block-scalar bodies too. The rest follows from
  it:
  - `PresetLoadResult` / the attach path return `{path, isTemplate,
    battery}`, not a bare path;
  - a shared `NamedFileCatalog`-level helper for lists / classify /
    save-name validation, leaving the managers only Qt glue;
  - one directory scan per refresh.
- **`saveas:` media is written by two pipelines.** The GUI managers
  (`MemoryModuleManager::saveSlotAs`, `FloppyDiskManager::saveDiskAs`)
  and `Core/PC1600/PC1600PresetMedia.hpp` for the CLIs each splice the card
  / format the floppy and write it. They share `namedFileName()` since
  04d83d3, nothing else. The Core copy is PC-1600 only, although
  `saveas: s1:` is valid on the PC-1500, and `pc1500_cli` has no saveas.
  Fix: one model-neutral Core function (card image + source text, or disk
  image, plus the request → text and path); the managers keep only the
  name/template policy and retargeting the autosave. Goes with the
  template-vs-instance entry above. The two managers' identical
  explicit-file / template refusal checks fold into it too.
- **PC-1600 live typing rides on the paste feeder.** `MachineController`
  runs PC-1600 live keys through the paste `KeyPasteFeeder` with a
  `m_liveTyping` flag that changes what `pasteActive()` means; every paste
  entry point (`enqueueTyped`, `cancelPaste`, `discardMachine`,
  `interruptPaste`) has to reset it. The PC-1500 has a Core key queue
  (`PC1500Machine::enqueueKey`). Fix: the same queue on `PC1600Machine`,
  expanding KBII there with `kbiiSequence()`; then the feeder loses its
  PC-1600-only `KbiiChar` action, `closesKbii` and `m_kbiiLatched`.
  **Before fixing:** the interrupt-a-paste-mid-KBII behaviour
  (`cancel(..., finishKbii)`) must survive the move; check it against the
  KBII entry in docs/background/Decisions.md.
- **`MachineCodeLoadDialog::refresh()` repeats `planLoad()`'s PC-1600
  steps** (the LH5803 range check, `lh5803ToZ80`, `pc1600TargetFor`). The
  dialog gets only the length, not the `File`. Fix: hand it the `File` and
  `LoadOptions`, call `planLoad()` and add `pc1600WorkAreaWarning` on top.
  **Before fixing:** the dialog's own texts ("The LH5803's RAM is
  &0000-&7FFF.") differ from `planLoad()`'s; decide which wording stays.
- **Host drive: small leftovers in `HostDirectoryDrive.hpp`.** A wildcard
  `doRename` calls `findOne()` (a folder listing) per matching file; check
  collisions against one name set instead. `doCreate` lists files, then
  directories: one pass could collect both. `doWrite` stats the file again
  after writing to decide on `resize_file`; the size before the write
  already tells.
- **`PC1600BasicTyper.cpp` `codePointCount` counts UTF-8 lead bytes by
  hand**; count with `decodeUtf8` (Core/Utf8.hpp) so malformed bytes count
  the way the typer then types them.
- **PC-1600 LCD / sub-CPU timing model** *(behaviour/timing)*.
  `PC1600Display::kBusyClocks = 4` and `PC1600SubCpu::kResponseMicros =
  1660` were both fitted to real-unit benchmarks on 2026-09-23 while the
  ~0.65 % BASIC-speed residual is still open (see the "PC-1600 BASIC runs
  ~0.65% fast" known issue), so each may partly compensate for it.

  **LCD half.** The HD61102 datasheet (Hitachi *LCD Controller/Driver LSI
  Data Book* U74, 1989, printed pp. 261–290; local copy
  `PC-1600/Hitachi_HD61102_1989.pdf`, HD61203 alongside) bounds the busy
  time at **1/fCLK ≤ T_BUSY ≤ 3/fCLK**, where fCLK is the φ1/φ2
  frequency. Confirmed from the Service Manual's key circuit diagram
  (printed p. 43): CK0 (CN1-40) drives the HD61203's CR pin with R and C
  open (external clock), and M/S = VCC (master), FS = GND, DS1 = GND,
  DS2 = VCC (1/64 duty). The HD61203 datasheet gives fosc = 215 kHz at
  FS = GND and fosc = 2 × fφ, so fCLK = 108.3 kHz and the bound is
  9.2–27.7 µs. The fit (busy until the 4th CK0 edge = 3–4 CK0 periods =
  13.8–18.5 µs, ~2 φ cycles) is inside it.

  Datasheet behaviour left out because it moves the fit. The scroll/copy
  routine (bank 6 `8A2C`/`8A66`) reads 4 bytes and writes 4 per column, so
  apply these only together with the re-fit, against the scrolling-PRINT
  benchmark:
  - Busy after data *reads* as well. The ROM busy-waits before every read
    (`8AA2`), which is consistent with this.
  - Busy phase-locked to φ: end on the 2nd φ edge (2–4 CK0 periods)
    instead of the 4th CK0 edge.
  - Instructions and data writes ignored while busy (only Status Read is
    accepted). The ROM always polls, so only user ML that doesn't would
    notice.

  **Sub-CPU half.** The 1.66 ms figure comes from the 0.5 s ISR's commands
  but is applied to every sub-CPU command (clock, IOCS, the power-off
  byte). It was fitted while SRIRQ's 0.5 s bit toggled, i.e. with the ISR
  doing its full work (SRIRQ A2H, SRA0 A8H, SRINP A3H) only every other
  tick. Now it does that every tick, and A−C comes out at 60 ms against
  the real 45 ms, so the per-command time is too long. Of the total,
  27.7 us is the CPC's own DSTB delay (now modelled from PR2/PR7); the
  sub-CPU's share is `PC1600SubCpu::kResponseMicros` = 1632. Re-fit once;
  consider a per-command response time. Verify with the scrolling-PRINT
  benchmark.

  **Order for the sub-CPU half:**
  1. ~~The TC8576F datasheet check~~ — done (dev-0.6.0): PSR BUSY and
     XBUSY are separate, the DSTB delay is modelled (27.7 us of the fit).
  2. ~~The sub-CPU protocol spec~~ — done in the corpus
     ([Ref/PC-1600/PC-1600-SubCPU-LU57813P.md](https://github.com/tinue/Sharp1500-1600-Ref/blob/main/PC-1600/PC-1600-SubCPU-LU57813P.md)): the 0.5 s ISR sends A2H, A8H, A3H.
  3. The residual itself. The `ON TIME$` hypothesis is ruled out by the
     ROM (see the known issue); next is timing the FOR/NEXT routine from
     RAM.
  4. Then re-fit once, against A−C = 45 ms.
- **The debugger parses the project preset twice per session action.**
  Attach, restart and Build & Load call `effectiveConfig` →
  `parsePresetFile`, then `loadPreset` / `cleanStart` parse the same file
  again by path (`PresetController::parsePreset`), each time re-reading
  every `program: file:`. One parse per clean start is deliberate
  (Decisions.md), two isn't. Fix: pass the `PresetFile` from
  `effectiveConfig` down (`DebugController` → `SyncOperations` →
  `PresetController::runPreset(preset)`).
- **Catalogue scans read `encoding: file` sidecar ROMs.**
  `MemoryCardCatalog.hpp` `parseEntry` passes `baseDir`, so every scan
  (`scanMemoryCardDirectory`, `readMemoryCardCatalogEntry`,
  `resolveModuleSpecByName`, every picker refresh) opens each sidecar just
  to fill the catalogue entry. No bundled card uses `encoding: file` yet.
  Fix: part of the catalogue parse mode in the template-vs-instance entry
  above (skip content). **Before fixing:** today a missing sidecar drops
  the card from the list; with a metadata-only parse it would be listed
  and fail on attach -- decide which is wanted.
- **Small leftovers from the 2026-09-30 simplify pass.** Take them when
  the file is next touched:
  - `--lcd-png` write-and-error block is the same in `pc1500_cli.cpp` and
    `pc1600_cli.cpp`: a `cli::writeLcdPng(bitmap, mm, path)` in
    `tools/CliCommon.hpp`.
  - `DropFile.cpp` `classify` and `PresetFile.cpp`'s `program:` switch
    both sort `programfile::Kind` into BASIC / code: `isBasic(Kind)` /
    `isCode(Kind)` next to `headerName()` in `ProgramFile.hpp` (the
    headerless rules stay with the callers).
  - `parsePresetFile` spells the top-level keys three times (`kKeys`, the
    `block` test, the dispatch chain): one `{name, isBlock}` table.
  - `parsePresetFile`'s `plotter` / `interfaceName` locals only copy into
    `out->` at the end; assign directly.
  - `SettingsDialog.cpp` `addOpenFolderRow` special-cases
    `OpenFolder::HostDrive` for the reset tooltip; pass the tooltip in
    like the label.
  - `extension.js`: `createDebugAdapterDescriptor` falls back to
    `setting('port') || 32168` although the resolver already filled
    `config.port`.
  - `tools/dap_smoke.py`: each run repeats Dap/initialize/attach/
    disconnect; a `session(port, **attach)` context manager.
- **PC-1600 module window base is guessed from the card image size.**
  `PC1600ProgramPlacement.cpp` (`windowContent`/`windowBase`) and
  `pc1600SlotGeometry` (`PC1600MachineCodeLoader.cpp`) take the image as
  top-justified in &8000-&BFFF (4 KB = &B000, 8 KB = &A000), while the
  writes go through the slot pins. CE-151 and CE-155 only agree because
  their sizes match their decode; a module whose RAM isn't top-justified
  would get its program written where the pins hold no RAM. Fix: derive
  the base from what the card answers through the pins
  (`pc1600LoadState`'s `bankRamPages` already scans that), and drop
  `SlotGeometry::imageSize`/`bankSize`. *(behaviour)* **Before fixing:**
  check every bundled PC-1600 card gives the same base both ways, and what
  the `windowContent(g) < 0x1000` check becomes.
- **The PC-1600 loaders route page C / page D themselves.**
  `PC1600BasicLoader.cpp` (`writeAt`) and `PC1600MachineCodeLoader.cpp`
  each pick slot bus vs. `debugWriteInternalRam(addr - 0xC000)` and split
  at &BFFF for the run-on; `MachineCodeFile.cpp` (`windowEnd = min(end,
  kPc1600S0Base)`) does the same arithmetic for planning. Fix: one
  `PC1600Memory::busWrite(bank, addr, data, n)` that resolves each address
  as the CPU would with page C = `bank` and page D = bank 0, so the
  boundary lives in one place (and a later page-D or SLOT1MAP change
  touches one spot).
- **`dev/loader-matrix/` re-implements Core helpers.** Take it when the
  matrix is next run:
  - `readFile` in both harnesses: `readWholeFile` (`Core/FileIO.hpp`).
  - `lcd()` and `screen.find("ERROR")`: `LcdText::logField()` /
    `LcdText::contains()`.
  - PC-1600 MEM is typed and parsed off the LCD:
    `CoreDebug::readPC1600ProgramAreas(peek).memS0`
    (`Core/Debug/BasicPointerTable.cpp`, add it to the build script).
  - `snapshot()` peeks C000-FFFF byte by byte:
    `PC1600Machine::debugCopyInternalRam()` (note that a peek at F07DH
    returns the Port 3DH mirror, not the RAM byte).
