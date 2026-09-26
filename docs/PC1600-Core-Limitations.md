# PC-1600 emulator core — known limitations

*Last updated 2026-09-18.*

A catalogue of the deliberate shortcuts, "not modelled" scope notes,
`TODO(trace)` markers, and unconfirmed assumptions in the emulator core
(`Core/`). Every entry is sourced from a comment in the code; the
`file:line` reference points at it.

Scope: this file covers the PC-1600 machine plus the CPU cores
(`LH5801`, `SC7852`, `LH5803`) and the connector / memory-card layer it
shares with the PC-1500/1500A, since a PC-1600 limitation is often really
a limitation of that shared code. PC-1500-only items are marked
*(PC-1500 side)*.

This is a "what the core does **not** do" list. It is not a bug list —
these are design boundaries, chosen to keep boot-to-BASIC and the
already-shipped behaviour deterministic and well-sourced. Revisit an
entry when a primary source or a ROM trace disagrees with it.

Some entries below are "used to be a limitation" notes — the memory-card /
Slot 2 work removed the gap — kept here because the surrounding area is
still worth understanding. They say what the code actually does now.

---

## CPU cores

### LH5801 — `Core/CPU/LH5801/`
*(PC-1500/1500A main CPU; also the base class for the LH5803.)*

- **`OFF` (opcode `0x4C`) not modelled** — the power-off request just
  costs 8 cycles and does nothing. `LH5801.cpp:921`
- **`CDV` (`0x8E`) clock divider not modelled** — treated as an 8-cycle
  nop. `LH5801.cpp:919`
- **NMI line not implemented** — the genuinely-non-maskable interrupt
  (vector `0xFFFC`) is "not modelled by this core at all yet"; flagged as
  a known gap, nothing found so far requires it. `LH5801.cpp:196`,
  `LH5801.cpp:319`
- **Undocumented opcodes execute as no-ops** — real-hardware behaviour
  for them "isn't sourced". A flag makes them distinguishable in the
  trace, but they do not halt. `LH5801.hpp:172`, `LH5801.cpp:722`
- **Timer while halted** advances by a fixed tick unit per `step()` call,
  not true crystal timing (deliberate, so `HLT`-then-timer-interrupt idle
  loops work). `LH5801.cpp:319`
- **Timer tick divisor (8 cycles / LFSR step)** was empirically
  recalibrated against a real unit's edit-cursor blink rate, not derived
  from a spec. `LH5801TimerTable.hpp:27`

### SC7852 / Z-80 — `Core/CPU/SC7852/`
*(PC-1600 main CPU.)*

- **Wait states: M1 only** — the TRM's "1 WAIT automatically inserted in
  the machine cycle" is modelled as one wait per M1 (opcode fetch /
  interrupt acknowledge), calibrated against real-hardware BEEP pitch (A=200
  and A=50 both within 0.22%). Whether I/O cycles get an extra wait on top
  of the Z-80's own is not modelled; the BEEP loop's 4 port accesses per
  period can't resolve it (<0.05%). `SC7852.hpp` (`kM1WaitStates`)
- **MEMPTR register not modelled** — the undocumented X/Y flag bits after
  `BIT n,(HL)` / `(IX+d)` / `(IY+d)` are approximated from the operand
  byte instead of the internal MEMPTR. `SC7852.cpp:721`, `SC7852.cpp:1054`
- **`DD`/`FD` `CB` undocumented register-copy side effect not modelled** —
  hardware also writes the rotate/shift/RES/SET result into the register
  named by the opcode's low 3 bits. `SC7852.cpp:1037`
- **Undocumented `ED` opcodes** treated as 4-T NOPs. `SC7852.cpp:917`
- **Registers not re-randomised on reset** — A/F and the GP registers keep
  their construction-time values; a deterministic simplification, not a
  claim about real silicon's power-up pattern. `SC7852.cpp:39`

### LH5803 — `Core/CPU/LH5803/`
*(PC-1600 sub-processor, PC-1500-compatibility CPU.)*

- **ME0 / ME1 as two distinct 64 KB spaces not modelled** — ME1 aliases
  ME0 except for the CPU-handoff trigger and the UART/sub-CPU register
  block. `LH5803Memory.hpp:33`, `LH5803SharedMemory.hpp:12`
- **`TODO(trace)`: which ME1 address form the OFF-path clock-save ROM
  actually uses** (`rom1500 E538`: literal `0x0021`/`0x0033` vs. the
  `0xA03x` shadow) is unconfirmed — both are routed the same way for
  safety. `LH5803SharedMemory.hpp:46`
- **Standalone `LH5803Memory` uses a private 16 KB RAM array** (Phase 5.2,
  isolation testing only); real cross-CPU RAM sharing is
  `LH5803SharedMemory` (Phase 5.3). `LH5803Memory.hpp:20`

---

## PC-1600 serial / UART (TC8576F)

`Core/PC1600/TC8576F.*`, `Core/Serial/SerialLink.hpp`, `Core/Serial/PtySerialLink.*`

The register model follows Toshiba's TC8576AF data sheet (SharpPC1500Reference
`PC-1600/PC-1600-CPC-TC8576.md`). Remaining gaps:

- **RS-232C / SIO connector mux not modelled** — the chip's PRIME output
  (the gate array's PRIM select) is tracked (`TC8576F::rs232Selected()`),
  but both connectors map to the one `SerialLink`.
- **Line timing is per character, not per bit** — the character time comes
  from PR7 and PR1:PR0 exactly, but no start/stop/parity bits are generated:
  a host transport carries bytes. `SerialLink::onBaud()` is advisory.
  Parity/framing errors and break (RBRK, SBRK) never occur.
- **Transmit queue deeper than the chip's double buffer** — 512 bytes, so a
  host bridge doesn't stall the ROM; TxRDY drops only when it is full.
- **/CTS (pin 35) and /DSR (pin 34) wiring unknown** — the transmitter
  follows the peer's CS; SSR bit 7 follows the peer's DSR (the ROM never
  reads it).
- **Parallel status-line interrupts (factor 2) not modelled** — the
  PC-1600 keeps IM2 set, so the ROM never sees them.
- **No `SerialLink` attached** → nothing connected: modem inputs read off,
  TxD is accepted and reported "sent" immediately, RxD never has data.
  Enough for the internal handshakes the ROM runs before any medium access
  (RAM disk, power-off) and keeps headless probes deterministic.
- **Raw PTY carries no RS-232C modem lines** — `PtySerialLink::getStatus()`
  reports CTS and DSR permanently asserted; DCD approximates "a peer
  currently holds the slave open". Non-POSIX build: every operation is a
  no-op and `isOpen()` is false.
- **Still open** (see `docs/PC1600-Serial-Port.md` and `TODO.md`): an
  end-to-end round-trip against real SharpDataExchange and the follow-on
  localhost-socket transport.

---

## PC-1600 sub-CPU (LU-57813P)

`Core/PC1600/PC1600SubCpu.*`

No datasheet and no ROM dump exist; the command set comes from the Z-80 ROM
(SharpPC1500Reference `PC-1600/PC-1600-SubCpu-LU57813P.md`). Modelled: the
clock, the wake-up and two alarm timers with minute-carry compare and `?`
wildcards, the interrupt mask/pending bits and INT6, the password, the reset
/ power-on cause, system off/on, and the handshake timing. Remaining gaps:

- **Commands no source explains** (IOCS 0CH–0FH, 16H/17H, 1BH–1FH, 26H, the
  LH-5803's DCH) are accepted and leave the previous answer standing.
- **Analog input / external keyboard** — the SWA1A thresholds and the
  IOCS 1EH mode are stored, but no analog-input interrupt or external-
  keyboard event is generated. The analog port (SRA1) is injected state
  (`setAnalogInput()`); nothing in the core drives it.
- **Supply voltages** (SRA0 main, SRA2 CE-1600P pack) always read C0H, clear
  of the ROM's low-battery thresholds. No low-battery (Q3) power-off.
- **F-pin tones not generated** — key click (SBEEP), the ALARM$ beep, the
  wake-up beep and the hour signal. Frequency and length are unmeasured
  (`TODO.md`).
- **February** follows the seeded year's calendar. The Service Manual says
  the chip has no leap-year handling, but not which February it keeps.
- **Response time** is one fitted figure for every command
  (`kResponseMicros`), see the timing item in `TODO.md`.
- **Clock is never read from the host clock inside Core** — kept
  deterministic for tests; the GUI/CLI layer injects a real "now". The
  emulated clock is paced off emulated time, so a seeded time drifts if the
  host cannot hold the emulation at real speed (exactly as a real PC-1600
  drifts against its own crystal).

---

## PC-1600 interrupts, timers, power management

`Core/PC1600/PC1600Machine.hpp`, `PC1600Memory.*`

- **Port 32H causes**: bit 0 (TC8576F INT) and bit 6 (sub-CPU Z7) are live
  levels, bit 3 (LH-5803 hand-back) and bit 4 (1/64 s timer) are latched and
  cleared by the 32H read. The other bits have no source.
- **Timer clock domain**: the 1/64 s signal and the sub-CPU's 0.5 s tick
  accumulate only SC-7852 T-states, so they pause while the LH-5803 owns
  the bus (MODE 1, the OFF sequence) and while the system is off. On
  hardware both come from the sub-CPU and never stop. The calendar clock
  and the timers don't have this gap: they run in every state.
- **Power**: modelled as on hardware (Decisions.md). The sub-CPU cuts power
  after the system-off command once the bus owner halts; the ON key, the
  wake-up timer (SWPON bit 1) and CI (SWPON bit 0) power on through a
  reset that reports the cause. The OFF key leaves FA08H = AAH (no
  resume), auto power-off leaves A5H plus the saved SP, and the ROM resumes
  from that. The PCTRL -> Q0 timing is not documented; "the bus owner
  halted" stands in for it. `reset()` always leaves the machine on, even
  from off; whether the real RESET switch powers the system on isn't
  documented (the Service Manual lists the CE-1600P's reset, KL, but not
  the RESET switch among the power-on sources).
- **No auto-power-off suppression / GUI affordance**: the ROM-level
  `KEYWK3` (F07BH) APO-suppress bit is not driven from Core, and there is
  no Settings toggle or OFF/ON UI control in the app layer.

---

## PC-1600 bank switching, slots, memory decode

`Core/PC1600/PC1600Bank.hpp`, `PC1600Memory.*`

- **SLOT1MAP (Port 3CH bit 2) is modelled** — when set, Slot 1's high 16KB
  half (beta) mirrors onto page-B bank 1 (4000-7FFF) in addition to its
  normal page-C bank-1 home, per the TRM's 0196H entry + diagram. Untested
  against real hardware or firmware: no card in this project's catalogue
  (CE-155, CE-1600M, CE-1620M) is banked or invokes 0196H, so coverage is
  synthetic port-write tests only, transcribing the documented truth table
  rather than cross-checked against an independent source. Collides with
  SLOT2MAP mode 2 at the same window (page-B bank 1) if firmware ever arms
  both; resolved by "last call wins" (`PC1600Bank::slot1MapWinsTie()`), an
  emulator-invented policy — real hardware's behaviour there is undocumented.
  `PC1600Bank.hpp:46`, `PC1600Memory.cpp` (`slot1MapTarget`,
  `resolveSlotMapCollision`)
- **Slot 2 vertical banking (Port 28H) *is* modelled** — on the card
  side. A `SoftwareDefinedCard` with a trigger-based latch samples the
  data bus on `OUT (28H)` and selects which 32 KB chip sits behind the
  8000-BFFF window, with PVOUT (Port 31H b4) as the nested 16 KB
  half-select. `Calc-U-1600/Resources/ce1601m.card.yaml` ships two vertical
  banks populated behind a 3-bit (D0-D2) latch; `superram.card.yaml` is the
  same card with a 4-bit (D0-D3) latch and all 16 x 32 KB banks fitted =
  512 KB. `PC1600Memory.cpp:307` (routes 28-2FH to the card as an
  `ioWrite`), `Core/tests/pc1600_slot_module_tests.cpp`
  (`test_trigger_latch_modules_in_slot2…`),
  `Core/tests/memory_card_tests.cpp` (`test_superram_16way_vertical_banking_direct`).
  What is *not* modelled: `PC1600Bank`'s own Port 28H latch is a bare
  readback register (`slot2VerticalBank()`), consulted only by the debug
  panel and the bank tests — the live decode is entirely card-side.
  `PC1600Bank.hpp:25`
- **CS24 8 KB sub-banking of Bank 3 (`A16A`) not separately modelled** —
  both Bank 3 / Bank 3b ROM images load as flat 16 KB blocks and the
  decoder does a whole-16 KB swap. `PC1600Memory.hpp:33`
- **Page B banks 4/5 = CE-1600P plotter/floppy ROM**: real, documented
  content, left open bus until the CE-1600P itself is emulated.
  `PC1600Memory.hpp:54`, `PC1600Memory.cpp:94`
- **Page C banks 0/1 (Slot 1) and 2/3 (Slot 2)**: open bus only when the
  slot is *empty*. Concrete modules now exist as software-defined cards
  — shipped card definitions `ce155.card.yaml`, `ce1600m.card.yaml`,
  `ce1601m.card.yaml` (each with its own `compatible-hosts`), plus the
  hardcoded prototype cards — and attach via a preset
  `memory-expansion-N:` block or the GUI control-bar module picker.
  `Core/Connector/SoftwareDefinedCard.hpp`, `Calc-U-1600/Resources/*.card.yaml`
- **Page D bank 1**: the weakest-evidenced cell in the whole
  bank-switching map (single-source, routing it to the external 60-pin
  system bus). Left open bus rather than built on that; revisit only if a
  primary source turns up. `PC1600Memory.hpp:70`, `PC1600Memory.cpp:122`
- **Page A bank 1** (a hypothetical second bank at `0000-3FFF`) is
  undocumented — only bank 0 content is ever loaded. `PC1600Memory.hpp`
  (page-A comment)
- **Most of the I/O map is unimplemented** — outside the ports the decode
  explicitly claims, every port reads open bus (`0xFF`) and ignores
  writes ("this class has no opinion on the rest of the I/O map — UART,
  timer/RTC, etc. all remain out of scope" for the standalone decoder).
  `PC1600Memory.hpp:96`
- **Keyboard strobe direction registers (DDA/DDB) not fully consulted** —
  KS0-7 are treated as "active when the OPA bit reads 0" regardless of
  DDA, the same simplification the PC-1500 side carries. (The PB6 strobe
  *does* consult DDB.6.) `PC1600Memory.hpp` (`m_dda` comment),
  `PC1600Keyboard.hpp:1`
- **PB7 (ON/BREAK) is not surfaced on port 1FH** — it reaches the ROM only
  through the 1BH interrupt-flag latch (`setOnKeyPressed()`); bit 7 of the
  1FH read stays 0. `PC1600Memory.hpp` (`m_pbPins` comment)
- **CE-1601M RAM disk (`S2:`) works, with a required order of operations**
  (resolved 2026-09-02, commit `5a9ada6`; verified end-to-end with `MEM`
  read off the rendered LCD). A virgin card must be **fully formatted
  first** with `INIT"S2:","F"`; only then can it be split with `"M"`
  (RAM/ROM file area) or `"P",n` (program memory, `n` KB from vertical
  bank 0). The one-step `INIT"S2:","P",n` / `"M"` on a *virgin* card is
  not a format command — this matches the ROM (rom3b `6464` only calls the
  format IOCS when `F421` bit 4 is set, which a virgin card leaves clear)
  — so it silently leaves vertical bank 1 blank and `FILES"S2:"` returns
  ERROR 160. `FILES` / `SAVE"S2:"` / `LOAD"S2:"` all work once formatted.
  See `examples/maxed-out-mem.pc1600` (runs the full `F` → `M` sequence).

---

## PC-1600 display (HD61203 + 2× HD61102)

`Core/PC1600/PC1600Display.*`, `PC1600StatusLine.hpp`

- **Controller busy time is fitted, not from a datasheet**: busy (status
  bit 7) holds until the 4th edge of the 216.7 kHz LCD clock after each
  write. That was chosen to match a real unit's scrolling-PRINT benchmark
  (see `PC1600Display.cpp` readIO). Status bit 5 (display on/off, Baum
  Systemhandbuch Anhang A) still always reads 0 ("on").
  `PC1600Display.hpp` (kBusyClocks)
- **`displaySL` (set-display-start-line, `0xC0-0xFF`) rotating-window
  offset**: modelled on the read side for the graphics-area scroll (it was
  found to be necessary), but the right 28-dot block's *further* rotation
  is "not modelled here". `PC1600Display.hpp:43`
- **Unrecognised controller commands** (including the write-side handling
  of `0xC0-0xFF`) are silently dropped — the "accepted, not modelled"
  stance. `PC1600Display.cpp:31`, `PC1600Display.cpp:118`
- **Status-symbol line** is read from display RAM (IC3 column 63, pages
  4/6/7), as the ROM's symbol writer (bank 6 8220H) stores it and the
  Service Manual glass pinout wires it. `PC1600StatusLine.hpp`
- **`Romaji` / `Kana` segments** (page 4 bit 2 / page 7 bit 2, commons
  X35 / X59): which part of the "ローマ字→カナ" caption each lights is
  *inferred* from pin order, and the GUI's `kana` position is estimated.
  The western ROM never sets either bit. `PC1600StatusLine.hpp`,
  `Qt6/app/LcdWidget.cpp`
- **DEG / RAD / GRAD modelled as three independent bits** even though the
  hardware has one physical legend; real firmware is *assumed* to only
  ever set one at a time. `PC1600StatusLine.hpp:58`

---

## PC-1500 memory / I/O *(PC-1500 side — shared LH5801 platform)*

`Core/PC1500/PC1500Memory.*`, `Upd1990ac.*`, `PC1500Keyboard.hpp`

- **LH5811 I/O controller: only the registers a stock PC-1500 needs are
  modelled** (DDA/OPA, DDB/OPB, and the uPD1990AC RTC bit-banged via
  OPC/PC0-PC5), plus the buzzer on OPC/PC6. **Serial transfer and the F
  register's modulated SDO output are out of scope**: no stock
  boot-to-idle or BASIC-editing behaviour depends on them. The CE-150 tape
  code would need them (F = 63H: 2539 / 1270 Hz). `PC1500Memory.hpp:52`
- **F / G / MSK registers and the unused register-select codes**: stored
  as plain read/write bytes defaulting to `0x00`. This does *not* model
  serial transfer or MSK's real interrupt-masking effect — but a real ROM
  both writes and reads them, so echoing what was written is a more
  faithful default than a constant. `PC1500Memory.hpp:231`,
  `PC1500Memory.cpp:169`, `PC1500Memory.cpp:201`
- **BREAK via the ON key** is reproduced by a direct write to IF bit 1 on
  the press transition. Real hardware most plausibly has the ON key's IRQ
  line OR-wired onto the same latch TP's rising edge sets — "plausible but
  unconfirmed by ROM tracing", and unconfirmable by disassembly since no
  software sets that bit. `PC1500Memory.hpp:114`
- **ME1 outside the I/O-chip decode window mirrors ME0** — a conservative
  Phase 1 placeholder, "flagged for revisit once Phase 4's
  ExpansionConnector work clarifies ME1's remaining role."
  `PC1500Memory.hpp:57`
- **ON-key wake-from-power-off path not modelled** — ON is wired outside
  the key matrix; "modeling that wake path is out of scope for now."
  `PC1500Keyboard.hpp:24`
- **uPD1990AC RTC**: test mode (C0-C2 select) "not modelled"; the live
  clock advances via a 1 Hz cycle accumulator, not the host wall-clock.
  `Upd1990ac.cpp:89`, `Upd1990ac.hpp:29`

---

## Expansion connector & memory cards

`Core/Connector/`

- **40-pin `ExpansionConnector` (PC-1500 side)**: concrete cards attach
  through it — the `SoftwareDefinedCard`, built from a `.card.yaml`
  definition and wired by `PC1500PresetLoader`. The GUI reads the
  attached module's name from the slot (`ExpansionCard::moduleName()`).
- **60-pin `SystemBus`: CMTIN / CMTOUT (cassette FSK audio), WEX / W1
  (external WAIT), INT, BFO / φOS** are named-but-unwired placeholder pins
  — "genuinely analog, out of scope until cassette support is built".
  Nothing consults them; they exist only so the pin model stays honest
  about all 60 pins. `SystemBus.hpp:26`
- **Assumption, not independently confirmed**: the 60-pin connector's
  pinout is identical between PC-1500 and PC-1500A, so all S1-S4 route
  unconditionally on both. Worth confirming against a second TRM scan.
  `SystemBus.hpp:33`
- **CE-1638 / CE-163F only work in Slot 2 on a PC-1600, not Slot 1.**
  Both cards decode a banked Y0 window from pin 4; on a PC-1600 they act
  as a plain unbanked 16K module (the boot ROM sizes them as +16384 and
  BASIC RAM base drops `C0C5H` → `80C5H`), which is exactly what happens
  in **Slot 2**, where pins 16-18 carry the dormant K0-K2 lines so the
  pin-18 bank strobe never fires. **Slot 1 is broken for these two
  cards**: there pin 18 carries S3, which the mainboard pulses on every
  write to `&B000-&B7FF`, so the boot ROM's RAM-sizing scan keeps
  tripping the bank latch (and walking it into the FLASH banks on
  CE-163F) — `MEM` shows no growth at all. Verified end to end with the
  real ROM set.
- **CE163F flash: DQ7/DQ6 toggle-bit status polling and
  software-ID / autoselect (`0x90`) deliberately not modelled** — erase
  and program complete instantaneously, so the firmware's poll loops exit
  naturally. `SoftwareDefinedCard::flashWrite()`
- **No disk persistence** — flash banks (and every other module's storage)
  are volatile, like the rest of this project's module storage. Battery
  backup is on the roadmap but deferred until peripherals exist.
- **`MemoryCardDefinition` v1 scope**: Regular content, with Unbanked or
  **trigger-based** Banked latches (the CE-1601M / CE163F path — this
  works). Still rejected by the definition loader: split (by-bank)
  content, line-based banking, and initial-content / ROM / Flash header
  images — each returns a clear "not supported in v1" error.
  `MemoryCardDefinition.hpp:17`, with rejections at `:415`, `:420`,
  `:491`, `:592`, `:600`
- **Slot 2 K0-K2 connector pins (16-18) not asserted** — harmless: the
  modelled CE-1601M reaches its vertical-bank latch off the Port 28H
  *data-bus write* instead (see the bank-switching section above), so
  nothing needs those pins driven. **Pin 2 (PVIN, the LH5803 PV line)** is
  likewise deasserted — PC-1500-compatibility CPU mode only.
  `MemorySlotConnector.hpp:159`

---

## Peripherals with no model at all

- **CE-1600P Ni-Cd pack / battery voltage** — no model (see PC-1600
  sub-CPU section above). The plotter mechanism and its floppy drive are
  now modeled; see the next section.
- **CE-158 on the PC-1600: input through `SETDEV KI` / `INPUT`** — in
  MODE 1 the CE-158 prints (parallel `OPN "LPRT"`, serial `SETDEV PO`)
  and `RINKEY$` reads its UART, but `SETDEV KI` + `INPUT` never reaches
  the CE-158's ROM (no UART access at all; INPUT takes the keyboard). The
  PC-1600 has its own native `SETDEV` (KI = F14EH b0), which likely takes
  the command; not yet checked against real hardware.
  `Core/Connector/Ce158Card.hpp`, `LH5803SharedMemory.cpp`
- **Cassette (CMT)** — connector pins exist, no FSK / audio path.
  `SystemBus.hpp:27`
- **Buzzer: partial.** Modelled: the PC-1500 OPC/PC6 line, the PC-1600 OPC
  b7/b6 line, and the PC-1600 F-register (17H) modulator in its idle case
  (SDO = FX, phi = 1.3 MHz / 4, measured). The PC-1600 also has an
  acoustic transducer model. Not modelled: serial transmit through
  L (16H), so SXO never leaves mark and FY is never heard; the G register
  (19H); and the buzzer's second input F from the sub-CPU (key click,
  alarm). `PiezoSampler.hpp`, `PC1600Memory.hpp` (m_fReg)

## CE-1600P plotter / CE-1600F floppy

`Core/Connector/CE1600PCard.hpp`, `Core/Connector/CE1600FCard.hpp`

- **Implemented**: page-B banks 4/5 ROM window, plotter motor-phase ports
  (0x81-0x83), and the CE-1600F's full register protocol (ports
  0x70-0x7F) — command/sector/motor-status/data registers, disk-changed
  latch, two-sided media, real motor-startup/access timing. CE-1600F
  attaches as a union with CE-1600P (there is no separate floppy
  attach/detach); persistence is a versioned `<name>.floppy.yaml`
  (`Connector/FloppyImageFile.hpp`) via `FloppyDiskManager`.
- **Resolved**: `INIT"X:"` formats a blank disk (prompts "Set diskette
  for X:", then formats on Enter); SAVE/LOAD/FILES/DSKF work end to end.
  The controller model tracks head position, holds busy across data
  transfers, and implements command 0x80 as READ ID -- see
  `docs/PC1600-CE1600F-Format-Handoff.md` §0.
- **Not modeled**: GCR (4/5) encoding (sector bytes are stored decoded,
  per the Service Manual's own note that this only matters at the
  flux/bitstream level), the FDU-250's solenoid/cam seek mechanics,
  per-track formatted/unformatted state (the image holds sector data only,
  so every track answers READ ID with standard IDs -- an unformatted disk
  is recognised at the filesystem level instead: `FILES` gives ERROR 161), and
  write-protect (`m_writeProtect` exists but nothing ever sets it true —
  the emulated drive is permanently writable).

---

## Preset loader

`Core/Preset/PresetFile.cpp`, `Core/PC1600/PC1600PresetLoader.cpp`

- **`check:` steps** not yet supported. `PresetFile.cpp:178`
- **`format: basic-tokenized`** not yet supported. `PresetFile.cpp:363`
- **`program: format: binary`** (machine-language loading) not supported
  for a PC-1500 preset; **binary program sections** not supported for a
  PC-1600 preset yet. `PresetFile.cpp:473`, `PC1600PresetLoader.cpp:167`
- **`rom-modules:`** unsupported / rejected. `PresetFile.hpp:143`
- **Tabs rejected** — 2-space indentation only. `PresetFile.cpp:95`
- **`format: basic-binary` can't straddle a bank boundary**: loading into
  an expansion-module program area that would cross a card's bank
  boundary (e.g. `NEW "S2:"` in a vertically-banked Slot-2 module) is out
  of scope; `PC1600ProgramPlacement`'s module-region placement is ready
  for it once a preset syntax exists to request it.
