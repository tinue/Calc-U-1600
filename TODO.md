# TODO

The living backlog: known bugs, unbuilt features, and pre-release
obligations.

## Known issues

- **PC-1600 BASIC runs ~0.65% fast vs a real unit (MODE 0); cause open.**
  Measured 2026-09-23 from audio recordings (BEEP markers, ms precision;
  the recorder reads 0.22% slow, calibrated from the F-register whistle).
  Every BASIC program is ~0.65% short:

  | Program | Real | Emulator |
  |---|---|---|
  | A: `FOR I=1 TO 2000:NEXT I` | 6.275 s | 6.235 s (−0.65%) |
  | B: 100 scrolling `PRINT`s | 5.894 s | 5.854 s (−0.68%) |
  | C: A with sub-CPU interrupt masked (`OUT 53,&1F`) | 6.230 s | 6.189 s (−0.65%) |
  | D: A with all interrupts masked (`OUT 53,0`) | 6.011 s | 5.974 s (−0.62%) |
  | dampflok.bas, whistle 1→9 | 65.281 s | 64.843 s (−0.67%) |

  The residual is ~70 T (~19 µs) per `FOR/NEXT` pass in A, C and D alike,
  i.e. a fixed cost per statement/pass rather than a percentage.

  **Found and fixed on the way** (dev-0.5.0):
  - One wait per M1 cycle (f05e42e): BEEP pitch at two A values.
  - Sub-CPU IOCS 25H fast-path probe (6d6318a): the BEEP repeat slips.
  - Sub-CPU response time of 1.66 ms per command byte (64538ae): A−C.
  - LCD busy until the 4th LCD-clock edge (c18411b): B.
  - The ON key's live PB7 level (5011e2b).

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

  **Current hypothesis: the real unit takes a longer per-statement path
  because of work-area state the emulator never produces.** The statement
  loop (P0-B0 3AC0H) makes extra calls depending on F127H (pending BASIC
  timer interrupts: b7 `WAKE$(0)`, b6 `ON TIME$`, b5 `ALARM$`; b5 →
  `CALL 3860H` every statement) and F88DH (set by a hidden-ROM statement
  pair at P1-B3b 41FEH/4201H; ≠0 → `CALL 41DAH` → `CURUDCHK` 0172H every
  statement). On the real unit a PEEK after benchmark A gave F127H=64
  (`ON TIME$` pending), F88DH=0, F88EH=2. The emulator can't produce
  these: the sub-CPU accepts `WAKE$`/`ALARM$`/`ON TIME$` and drops them
  (see the RTC wake-up feature item). Bit 6 is *not* what 3AC0H tests
  (`AND 20H`), so next: find where bit 6 is acted on (e.g. 243FH/3E39H,
  called just before 3AC0H), and re-time benchmark A on the real unit
  with the `ON TIME$` request cleared. If the residual disappears, it's
  this. If not, copy the FOR/NEXT arithmetic routine into RAM and time it
  there. Forcing F127H/F88DH by POKE in the emulator hangs or changes
  program flow, so it's no substitute for the real mechanism.

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

## PC-1600 serial port

- Confirm PSR bit polarity/positions (the SIO overlay's CS/CD/DS bits;
  `CI`/ring is not currently surfaced) and which SSR bit the ROM
  readiness poll actually gates on, against a ROM trace.
- Whether the PC-1600 gates its own transmitter on incoming CTS (assumed
  yes today, CTS defaults asserted).
- Capture the exact on-wire `SAVE"COM1:"`/`LOAD"COM1:"` framing from a
  real ROM trace, and do an end-to-end round-trip against real
  SharpDataExchange.
- Follow-on: a localhost-socket transport (`SocketSerialLink`) so
  `OUTSTAT 0-3` and buffer-full RTS become effective end-to-end, for
  serial-only tooling to bridge via `socat`.

## PC-1600 program loading and pointer bookkeeping

The fast loader pokes BASPRG_END and the PRGADR end triple (`$FE3F`,
7b327cd) by hand, handles S0 only, and repeats the LH5803↔Z80 `+0x8000`
mapping in `PC1600BasicLoader`, `PC1600BasicTyper` (`toZ80`) and
`PC1600ProgramPlacement`. PRGADR was found missing late, so the real risk
is other work-area bookkeeping the loader skips. **Trace the scenarios
below on the ROM before deciding anything** (a pointer helper, or
mirroring the ROM's own sequence).

### Scenarios to trace
- **A memory module split into program area and base memory**
  (`INIT"Sx:","P",n`). What goes into ADTBL, SxMTb/SxMBb, the S1/S2
  descriptors (F019–F01E / F023–F028) and the module header, and how S0's
  bank list changes.
- **Loading a BASIC program into a program area** (TITLE = S1/S2). Not
  supported by the fast loader today. The ROM's `LOAD` finish takes a
  separate S1/S2 path (see below).
- **Loading a program saved on a PC-1500**, both machine code and BASIC.
  The PC-1600 accepts both. Machine code lands in LH5803-visible memory.
  Where BASIC goes, and whether it's converted or retokenised, is unknown.
- **Loading while the calculator is in MODE 1** (LH5803 owns the bus).
- For each scenario: diff the whole work area (F000–FFFF) typed vs
  loaded vs fast-loaded (same preset, `pc1600_cli`). Include a program
  that crosses a bank boundary, to settle the F02C question below.

### Found in the ROM so far (2026-09-26)
- **PRGADR** (jump table 02F4H = `RST 18H` → romI-0 188BH) only derives
  FE3C–FE41 and writes nothing else. With TITLE (F1D5) = 0 (S0):
  FE3C/FE3D = F865 (BE → LE, bit 15 set), FE3E = F02B (start bank),
  FE3F/FE40 = F867 (same conversion), FE41 = F02C (end bank). For S1/S2 it
  copies the 6-byte descriptor F019–F01E / F023–F028 to FE3C. If that
  slot's SxMTb (F016/F020) is ≥ FEh (not a program module), it resets
  TITLE to 0 and takes the S0 path.
- **romI-0 1874H** ("program empty?") compares FE3E with FE41, then
  FE3C with FE3F, so the start and end banks can differ.
- **`LOAD` finish, rom3b 70E1H**: the ROM's own "program placed in
  memory" sequence, with the end in FA00/FA01 and the end bank in FA02:
  1. writes the `FF` terminator at the end;
  2. S0: F867 = end (BE), **F02C = end bank**;
  3. S0: if the variable pointer F899 ≤ the new end (LOGEND, 02DAH),
     sets F899 = (F864):00;
  4. S1/S2 instead: end triple → F01C / F026, patches the module header
     (+5/+6, clears b7 of +7 when F3DB b1 is set);
  5. calls PRGADR;
  6. F89E (CURRENT TOP) = start, F1C1 (CURRENT bank) = FE3E;
  7. FA02 = FFh.
- **`LOAD` start, rom3b 71B3H**, fills FA00–FA05 from F865/F02B/F867/F02C
  (S0) or copies the S1/S2 descriptor.
- **Writers of F02C**: rom3b 44BCH/44CBH, 65E4H (`NEW`: F02B = F02C =
  F02AH), 70F8H (`LOAD` finish), and romce1600-2 34E3H (not looked at yet).
- **Gaps in the fast loader** compared with 70E1H: it never writes F02C,
  and it copies FE41 from FE3E instead of taking the end bank. That is
  correct only while the end stays in the start bank. The loader comment
  says that held on a measured program spilling into internal RAM, but the
  ROM treats the two banks as independent. A MODE switch re-runs PRGADR
  and propagates a stale F02C. It also skips F899, F89E and F1C1, which is
  probably harmless after NEW0 but unconfirmed.

## Feature ideas

- **Real-time-clock timers in the sub-CPU: wake-up (`WAKE$`), `ALARM$`,
  `ON TIME$`.** The real PC-1600 can switch itself on at a set time and
  raise BASIC timer interrupts. The emulated sub-CPU stores the clock but
  accepts and drops the timer commands (67H/69H/6BH). Its interrupt line
  only carries the 0.5 s tick, and its buzzer input F (click/wake-up/alarm
  tones) isn't modelled. Needed:
  - store the three timers;
  - raise the sub-CPU interrupt cause bits (§7.1: b7 wake-up, b6 alarm 1,
    b5 alarm 2);
  - power on from OFF at the wake-up time;
  - drive F127H's pending bits through the ROM's own handlers;
  - sound the alarm on the F path.

  Possibly also the source of the remaining ~0.65% BASIC timing gap
  (Known issues).

- What is the "second program memory" for BASIC programs on the PC-1600
  (relevant for ROM modules and battery-backed RAM modules)?
- Research MODE 1 (LH-5803/PC-1500-compat mode): does it genuinely reuse
  the old ROM for things like `PRINT`?
- CE-158 together with the CE-1600P. The real CE-1600P has its own
  connector at the back (like the CE-150), so both can be attached at
  once; today `PC1600Machine::attachCE1600P`/`attachCE158` detach each
  other and the preset parser rejects the pair (User Guide says "not yet
  supported").
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
  "eject first" (`docs/Floppy-Image-Format.md` §8).

## Code cleanup backlog

Refactors and internal costs, not user-facing bugs. Take them when the
area is next touched; entries marked *(behaviour/timing)* change what the
emulator does and need a deliberate check. Entries with a **Before
fixing** step need that analysis first. A fix that only moves the cost
somewhere else doesn't count (see docs/Code-Cleanup-Plan.md).

- **Picker and peripheral-button plumbing is written twice.**
  - `CE1600PRomVersion` clones `PC1600RomVersion`, and
    `BundledRoms::isCE1600PRomVersion` == `isPC1600RomVersion`.
  - The enum↔string mapping is inlined in `MachineController.cpp`
    (`versionName` lambda, `loadPC1600RomSet`) and `PresetController.cpp`.
  - The old-ROM fallback `QMessageBox` is copied.
  - MainWindow's ROM menu builder/sync and ControlBar's combo are
    copy-pasted, and the four `apply*Selection` handlers each carry an
    "already checked" guard (dff5d13).
  - `ControlBar::setCe150State`/`setCe158State`, and
    `MachineController::serialLinkStatus`/`ce158SerialLinkStatus`, are
    pairs.

  Fix: one `NewOldRom` enum with to/from-string, one
  `warnOldRomFallback()`, one menu/combo builder parameterised by label and
  slot, one peripheral-button setter, and one static status helper over a
  `PtySerialLink*`.

  **Design constraint:** the four "already checked" guards must end up as
  *one* guard inside the shared picker (call onPick only when the value
  changes). Replacing them with `toggled(true)` plus `QSignalBlocker`s in
  every sync setter only moves them.
- **Connectors.** `MemorySlotConnector` and `ExpansionConnector` share ~25
  lines of copy-pasted dispatch shell (a small base class would hold it),
  and `PC1500Memory` holds its connector by raw pointer injected by
  `PC1500Machine` where `PC1600Memory` owns its connectors by value.

  **Before fixing:** find out why `PC1500Memory` gets both its
  `ExpansionConnector` *and* its `SystemBus` injected by raw pointer (tests
  that build a bare `PC1500Memory`? construction order in `PC1500Machine`?).
  Converge the connectors and the bus ownership in one pass. Moving only
  the connector would leave the same inconsistency on the bus.
- **Card/floppy template-vs-instance rules are written twice and re-parse
  files.** `MemoryModuleManager` (`moduleLists`, `classifySlot`,
  `templateNames`, `saveSlotAs`) and `FloppyDiskManager` (`diskLists`,
  `classifySource`, `saveDiskAs`) each hold bundled-first shadowing, the
  template/instance split, the Name & Save checks and the "never
  autosave under the bundle" rule. They also re-read files Core just
  parsed: `classifySlot` fully parses the `.card.yaml` again (MB of
  `initial-content` hex for superRAM 512K) on every rebuild and twice per
  preset load; `saveSlotAs` parses the instance dir 3x; and
  `refreshModuleCombos` scans + parses both dirs once per slot.

  **Before fixing:** analyse `MemoryCardDefinition`'s parser.
  `scanFloppyDirectory`'s trick (cut the text at `\nsides:`) relies on key
  order, which hand-written `.card.yaml` files don't guarantee, and
  `isRom()` may need the regions. The cost is decoding the
  `initial-content` hex, so the likely target is a parse mode that skips
  that decode. The rest follows from it:
  - `PresetLoadResult` / the attach path return `{path, isTemplate,
    battery}`, not a bare path;
  - a shared `NamedFileCatalog`-level helper for lists / classify /
    save-name validation, leaving the managers only Qt glue;
  - one directory scan per refresh.
- **TC8576F interrupt plumbing.** Four `read/write/tick/reset` →
  `…Impl()` wrappers exist only to call `refreshInterruptOutput()`; the
  `std::function<void(bool)>` hook's one subscriber ignores the bool.

  **Before fixing:** check how the SC7852 samples INT (today
  `PC1600Memory::updateIntLine()` pushes it via `setIntLine()`), and what
  it would cost to read the level (`intCause() & 35H`, the UART's
  `interruptOutput()` as a const expression) when the CPU checks for
  interrupts. Target: nothing pushes updates. Moving the four wrappers
  into `PC1600Memory` (UART access / tick / relink / reset each calling
  `updateIntLine()`) only moves them.
- **PC-1600 LCD / sub-CPU timing model** *(behaviour/timing)*.
  `PC1600Display::kBusyClocks = 4` and `PC1600SubCpu::kResponseMicros =
  1660` were both fitted to real-unit benchmarks on 2026-09-23 while the
  ~0.65 % BASIC-speed residual is still open (see the "PC-1600 BASIC runs
  ~0.65% fast" known issue), so each may partly compensate for it. The
  HD61102 datasheet bound on busy time should be checked against the
  fitted 4 clocks; the 1.66 ms figure comes from the 0.5 s ISR's commands
  but is applied to every sub-CPU command (key scan, clock, IOCS). Chase
  the residual first, then re-fit once; consider a per-command response
  time. Verify with the scrolling-PRINT benchmark.
