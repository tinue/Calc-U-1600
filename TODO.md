# TODO

The living backlog: known bugs, unbuilt features, and pre-release
obligations.

## Known issues

- **PC-1600 BASIC runs ~0.4-0.65% fast vs a real unit (MODE 0); cause open.**
  Measured 2026-09-23 from audio recordings (BEEP markers, ms precision;
  the recorder reads 0.22% slow, calibrated from the F-register whistle).
  Every BASIC program is ~0.65% short:

  | Program | Real | Emulator (dev-0.5.0) | Emulator (2026-09-26) | Emulator (2026-10-07) |
  |---|---|---|---|---|
  | A: `FOR I=1 TO 2000:NEXT I` | 6.275 s | 6.235 s (−0.65%) | 6.251 s (−0.38%) | 6.250 s (−0.39%) |
  | B: 100 scrolling `PRINT`s | 5.894 s | 5.854 s (−0.68%) | 5.868 s (−0.44%) | 5.869 s (−0.43%) |
  | C: A with sub-CPU interrupt masked (`OUT 53,&1F`) | 6.230 s | 6.189 s (−0.65%) | 6.191 s (−0.63%) | 6.190 s (−0.64%) |
  | D: A with all interrupts masked (`OUT 53,0`) | 6.011 s | 5.974 s (−0.62%) | 5.974 s (−0.62%) | 5.974 s (−0.62%) |
  | dampflok.bas, whistle 1→9 | 65.281 s | 64.843 s (−0.67%) | not re-run | 65.229 s (−0.08%) |

  2026-10-07: dampflok is timed from the F-register whistle onsets (port
  17H bit 6 rising, `headless/beep/whistleprobe.cpp`). The same probe on
  the dev-0.5.0 tree gives 64.847 s, so it matches the audio method. Of
  dampflok's move, 8858e75 (64 Hz timer under the LH5803) is only +10 ms
  (65.219 s just before it); the rest came with the dev-0.6.0 sub-CPU work.
  So a program with LH5803 calls, machine code and sound is now within
  0.1%, while the pure `FOR/NEXT` benchmarks A-D are unchanged.

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

## PC-1600 LH-5803 / SC-7852 interface: still open

Background: Ref/PC-1600/PC-1600-CPU-LH5803-Compat.md §2a and §6.1,
docs/developer/PC1600-Core-Limitations.md (LH5803 section).

- **9400H trap decode (LHNMIO, SC-7852 pin 92).** Modelled as the opcode
  fetch at exactly 9400H with PU = PV = 1. The Service Manual says
  "94\*\*H"; open whether it is that one address or the step from 93FFH
  into 9400H, and whether port 30H b0 gates it. Hardware test in MODE 1
  without a CE-158: an LH-5803 routine `SPU`, `SPV`, jump to 9400H vs.
  9401H; again with `RPU`; again with port 30H b0 cleared. A marker byte
  written by C440H's path shows whether the NMI ran.
- **NMI edge vs. level.** The TRM (pin 15) reads like a level input; the
  core takes one NMI per request (edge). Works because the handler
  acknowledges at port 36H. Confirm or model the level.
- **Trap path timing.** In the emulator the per-byte display mirror is
  free, so the trapped scroll (handler + full `LCD1500_ALL` redraw, ~28k
  LH-5803 cycles) runs about one character behind the untrapped one in a
  TERMINAL session. The real cost of the mirror, and what port 30H b0
  really switches, are unknown.
- **LH-5803 maskable interrupt not driven.** MI (vector FFF8H) from the
  SC-7852's LHMIO (pin 91), and the PC-1500 peripheral IRQ (pin 80,
  "interrupt to the CPU (Z-80, LH-5803)") with IF/MSK at ME1 F00BH/F00AH,
  are not modelled. Nothing found so far needs them; the CE-158 ROM
  polls its UART.
- **ME1 wait states** (LHWAIT: `**0*H` and 8000H–FFFFH of ME1; IOE:
  `**00H–**0FH`) not modelled. The card decode also offers every ME1
  8000H–FFFFH access to the 60-pin cards, wider than IOE's window.
- **IN0–IN7 bit order.** The F.P.C. diagram (TRM p.264) puts the LH-5803's
  IN lines on the KIN net, but the photo doesn't resolve which IN goes to
  which KIN; INn = KINn is assumed. The TERMINAL menu's F4 decodes right,
  which fits, but one key is weak evidence. Check on the diagram or with a
  key test in MODE 1 (`INKEY$`-style ML via `ITA`).
- **CE-158 terminal: the first received byte is lost** (both TERMINAL and
  DTE show "B…" for a peer sending "AB…", with `--ce158-rx-hold`).
  Probably the receiver flush when the terminal starts; compare with a
  real unit.

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
- **Guide screenshots write into the real saves folder:** the chapter-5
  preset (`docs/developer/screenshots/presets/pc1600-modules.pc1600`) uses
  `saveas: live slot-1:My programs`, so every `tools/make_screenshots.sh`
  run writes a live "My programs" card into the Battery-card saves folder.
## Expansion connectors: one model on both machines

Done 2026-10-07 (docs/background/plans/Sixty-Pin-Connector-Plan.md, after the
2026-09-26 groundwork in Expansion-Connectors-Plan.md): each plug is numbered by
its real contacts. The 40-pin cards decode `PinState` (PC-1500: 2 = PU, 3 = PV, as
measured). The 60-pin cards decode `SystemBusPins` (`Contact60`: PT 14, PU 15, PV 16,
IORQ 26, MREQ 49, DME0 56, DME1/ELH̄ 58, ME1/IOE 59). There is one `SystemBus` per
machine. On the PC-1600 both CPUs drive it (`PC1600BusDrive`), and every
peripheral sits on it: CE-150, CE-158, CE-1600P/F, the host drive and bus ROMs.
Decisions.md, "One 60-pin connector per machine".

**Still open (hardware questions; the drive leaves these contacts inactive):**
- **Does the CE-1600P's I/O decode see LH5803 ME1 cycles?** While ELH̄ is
  asserted, IORQ carries the LH5803's ME1, so an access such as ME1 D070H
  puts 70H on A0-A7. The SC7852-side cards ignore every ELH̄ cycle. The
  Service Manual documents that for the ROM select CSNO; for IO7N and
  80H-83H it is assumed. This decides whether a CE-158 (registers at ME1
  D000-D3FF) and a CE-1600P can share the bus. **Before fixing:** probe
  IO7N (CE-1600P gate array pin 42) on a real unit while a MODE 1 program
  does `PEEK#` at an ME1 address with low byte 70H.
- DME0 (56) on Z-80 cycles, M1 (10) on memory cycles, PT/PU/PVOUT on I/O
  cycles: not documented. A logic probe on the 60-pin plug while stepping
  Port 31H through the page-1 banks would also confirm the PT/PU/PVOUT
  bank decode (Ref/PC-1600/PC-1600-Expansion-Bus.md §1).
- The PU drive handover between the SC7852 and the LH5803 (one shared line)
  is not measured.

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
  other and the preset parser rejects the pair. Both now sit on the one
  60-pin `SystemBus`. Blocked on the IO7N question in "Expansion connectors"
  (User Guide says "not yet supported").
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

  The connector work this waited for is done (2026-10-07): every
  peripheral sits on the one 60-pin `SystemBus`. Target: one button setter
  and one attach/detach path per peripheral kind, with "enabled" derived
  from what the bus can take instead of hard-coded pairs. The CE-158 +
  CE-1600P exclusion stays until the IO7N question ("Expansion
  connectors") is answered, so keep it as data the shared path reads, not
  as another pairwise rule.
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
- **PC-1600 module window base: follow the ROM's probe, not the image
  size.** `PC1600ProgramPlacement.cpp` (`windowContent`/`windowBase`) and
  `pc1600SlotGeometry` (`PC1600MachineCodeLoader.cpp`) take the base as
  &C000 minus the card image's (bank) size. The ROM's module map (rom3b
  67B2-6892) instead takes the first of 8000H / A000H / B000H in Slot 1,
  8000H / A000H in Slot 2, where `MEMORYCHK` finds RAM, and always runs the
  area to BFFFH (Decisions.md, "A PC-1600 module window starts at 8000H,
  A000H or B000H"). Both give the same base for every real and bundled
  module (4 KB B000H, 8 KB A000H, 16/32 KB 8000H); they would differ only
  for a module that never existed (e.g. 12 KB at 9000H: ROM A000H, loader
  9000H). Odd banks (2nd half of a 32 KB module) are mapped at 8000H only
  (`SMAPPAIR` 6898H). Low priority, no behaviour change for existing cards. Fix: probe
  the three bases through the slot pins like the ROM (`pc1600LoadState`'s
  `bankRamPages` already scans what the pins answer) and drop
  `SlotGeometry::imageSize`/`bankSize`; the `windowContent(g) < 0x1000`
  check becomes "no RAM at any base".
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
    `PC1600Machine::debugCopyInternalRam()`.
