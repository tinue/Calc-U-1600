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

- **Wait states not modelled** — every instruction uses nominal Zilog
  T-state counts. The TRM's "1 WAIT automatically inserted in the machine
  cycle" note has unresolved scope (every M-cycle vs. I/O-cycle only) and
  is ignored; this matches the documentation's own stated fallback and the
  reference emulator's choice. `SC7852.cpp:461` (`TODO(wait-state-scope)`),
  `SC7852.hpp:46`
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

`Core/PC1600/TC8576F.*`, `SerialLink.hpp`, `PtySerialLink.*`

- **RS-232C / SIO connector mux not modelled** — "RS-232C and SIO share
  the one channel and cannot be used at once… the RS-232C/SIO connector
  mux and real baud hardware remain a later refinement." This is the
  COM1-vs-SIO ("COM2") split. `TC8576F.hpp:12`, `TC8576F.hpp:55`
- **Real baud-rate hardware not modelled** — pacing is the emulator's own
  character-time approximation derived from the `pr[0]`/`pr[1]` divisor;
  `SerialLink::onBaud()` is advisory and transports ignore it.
  `TC8576F.hpp:55`, `SerialLink.hpp:55`
- **SSR / PSR status-bit semantics are partly unconfirmed guesses, not
  traced** — bits marked `TODO(trace)`: which SSR bit the ROM readiness
  poll (romIV-6 `A8F0`/`A974`) and the LH-5803 OFF loop (`rom1500 E538`)
  actually gate on; PSR bit positions and polarity, including the SIO
  overlay's CS/CD/DS bits (`TC8576F.cpp`) and whether the PC-1600 gates
  its own transmitter on incoming CTS. `TC8576F.hpp:42`, `TC8576F.hpp:141`,
  `TC8576F.cpp:76`, `TC8576F.cpp:84`
- **No `SerialLink` attached** → Phase-1 fallback: TxD is accepted and
  reported "sent" immediately, RxD never has data. Enough for the internal
  handshakes the ROM runs before any medium access (RAM disk, power-off
  clock save) and keeps headless probes deterministic. `TC8576F.hpp:44`
- **Parallel (Centronics) printer port**: FAULT / SLCT / PE / PRIME /
  XBUSY / IntF status lines have no model; the low printer-status bits are
  reused to surface connector inputs. `TC8576F.cpp:78`
- **Raw PTY carries no RS-232C modem lines** — `PtySerialLink::getStatus()`
  reports CTS and DSR permanently asserted; DCD approximates "a peer
  currently holds the slave open". Non-POSIX build: every operation is a
  no-op and `isOpen()` is false. `PtySerialLink.hpp:28`
- **Still open** (see `docs/PC1600-Serial-Port.md` and `TODO.md`): a
  ROM-trace capture of the real `SAVE"COM1:"` / `LOAD"COM1:"` framing, an
  end-to-end round-trip against real SharpDataExchange, and the follow-on
  localhost-socket transport.

---

## PC-1600 sub-CPU (LU-57813P)

`Core/PC1600/PC1600SubCpu.*`

- **The sub-CPU only answers what the boot ROM is known to check**; the
  stateful features behind those answers are deliberately inert:
  - **Password** — stored and compared but never gates anything.
    `PC1600SubCpu.hpp:41`
  - **`ALARM$` / `WAKE$` / `TIME_CHECK$`** — accepted and discarded; no
    source describes their real handling in enough detail to model.
    `PC1600SubCpu.hpp:41`, `PC1600SubCpu.cpp:84`, `PC1600SubCpu.cpp:169`
  - **`ON ADIN` sequences** — begin/end commands accepted, inert.
    `PC1600SubCpu.hpp:34`
- **Analog input port** (`setAnalogInput()`, request `06H`) is plain
  injected state — no real battery / voltage model, and nothing in the
  core drives it. `PC1600SubCpu.hpp:149`, `PC1600SubCpu.cpp:106`
- **Battery voltage** (request `55H`, CE-1600P Ni-Cd pack) — no model.
  `PC1600SubCpu.cpp:102`
- **Clock is never read from the host clock inside Core** — kept
  deterministic for tests; the GUI/CLI layer injects a real "now". The
  emulated clock is paced off emulated SC-7852 time, so a seeded time
  drifts if the host cannot hold the emulation at real speed (exactly as
  a real PC-1600 drifts against its own crystal). `PC1600SubCpu.hpp:47`,
  `PC1600Machine.hpp` (RTC accumulator comment)

---

## PC-1600 interrupts, timers, power management

`Core/PC1600/PC1600Machine.hpp`, `PC1600Memory.*`

- **Sub-CPU aggregate interrupt (INT6, port 32H bit 6): only the 0.5 s
  housekeeping timer is modelled.** Everything else that shares that one
  line stays unraised because nothing generates it —
  **low-battery, analog-in, CI line, auto-power-off, RS-232C timeout,
  and the wakeup / alarm1 / alarm2 timers.** `PC1600Machine.hpp:352`
- **Port 32H / 35H interrupt cause & mask**: stored but "not yet wired to
  any real interrupt source"; only bit 4 (the 1/64 s timer) is driven.
  `PC1600Memory.hpp:96`, `PC1600Memory.hpp:395`
- **Timer clock domain**: the 1/64 s timer, the 0.5 s timer and the RTC
  all accumulate **only SC7852 T-states** (not LH5803 cycles), so they
  effectively pause while the SC7852 is parked — "a real but small
  deviation from true hardware's always-running crystal."
  `PC1600Machine.hpp` (`m_timer64Accum` comment)
- **Auto-power-off / power management**: no explicit `m_poweredOff` state
  is modelled, and none is needed — OFF/APO power-down and ON-key wake-up
  fall out of the existing HALT + bus-arbiter behavior with zero special
  casing. The real ROM's OFF/APO shutdown sequence runs to completion and
  parks both CPUs in genuine HALT with bus ownership handed to the LH5803
  (`PC1600BusArbiter`); `PC1600Machine::setOnKeyPressed()` wakes whichever
  CPU is parked on the rising edge, regardless of which one owns the bus.
  The screen-clear-on-OFF vs. retain-on-APO distinction is likewise
  emergent: it's the real ROM choosing whether to clear display RAM before
  its display-off/on commands, and `PC1600Display` already retains pixel
  and status-symbol RAM across the "off" state, matching the **VGG power
  rail** (RTC, sub-CPU, internal-RAM retention, HD61102 display latch)
  staying powered while only **VCC** (CPUs, ROM, UART, LCD driver) drops.
  The RTC (`m_rtcAccum`) keeps advancing while the LH5803 owns the bus for
  exactly this reason.
  - **Reset-cause byte** (sub-CPU request `5AH`, `PC1600SubCpu::request`
    case `0x0A`, field `m_resetCause`): the boot ROM reads it at
    `romI-0 0x0346`, bit-shuffles it into `FA1BH`, and branches on the
    result. Bit 5 set = ALL RESET (wipes clock + internal-RAM work area +
    settings); clear = preserve them — verified by sweeping the value
    against the real ROM. `setResetCauseAllReset()` = `0xA0`,
    `setResetCauseSimple()` = `0x80`. The start-cause bitfield mirrored at
    `F1ABH`: `b0`=ALL RESET, `b1`=internal RESET, `b2`=external RESET,
    `b4`=POWER ON (ON key), `b5`=external power-on, `b6`=WAKE$(0), `b7`=CI.
  - **No auto-power-off suppression / GUI affordance**: the ROM-level
    `KEYWK3` (F07BH) APO-suppress bit is not driven from Core, and there
    is no Settings toggle or OFF/ON UI control in the app layer.

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

- **No controller-busy timing model** — a status/instruction read always
  reports the busy bit (bit 7) clear. This is load-bearing, not cosmetic:
  the boot ROM busy-waits on exactly this read before drawing.
  `PC1600Display.cpp:37`
- **`displaySL` (set-display-start-line, `0xC0-0xFF`) rotating-window
  offset**: modelled on the read side for the graphics-area scroll (it was
  found to be necessary), but the right 28-dot block's *further* rotation
  is "not modelled here". `PC1600Display.hpp:43`
- **Unrecognised controller commands** (including the write-side handling
  of `0xC0-0xFF`) are silently dropped — the "accepted, not modelled"
  stance. `PC1600Display.cpp:31`, `PC1600Display.cpp:118`
- **Status-symbol line: the software-side port/bit protocol is still
  unknown** — the flags are recomputed from display RAM (IC3 column 63,
  pages 4/6/7) after every write that could have touched that cell, rather
  than driven by a traced command path. `PC1600Display.hpp:52`,
  `PC1600StatusLine.hpp:71`
- **`Kbii` status segment** is read straight off panel bit 7 and simply
  stays false for the life of a western session; a Japanese machine's
  firmware is *assumed* to drive bit 7 for kana-entry mode where this ROM
  folds it into `S`. `PC1600StatusLine.hpp:37`, `PC1600Display.cpp:223`
- **DEG / RAD / GRAD modelled as three independent bits** even though the
  hardware has one physical legend; real firmware is *assumed* to only
  ever set one at a time. `PC1600StatusLine.hpp:58`

---

## PC-1500 memory / I/O *(PC-1500 side — shared LH5801 platform)*

`Core/PC1500/PC1500Memory.*`, `Upd1990ac.*`, `PC1500Keyboard.hpp`

- **LH5811 I/O controller: only the registers a stock PC-1500 needs are
  modelled** (DDA/OPA, DDB/OPB, and the uPD1990AC RTC bit-banged via
  OPC/PC0-PC5). **Serial transfer and the buzzer (OPC/PC6) are out of
  scope** — no stock boot-to-idle or BASIC-editing behaviour depends on
  them. `PC1500Memory.hpp:52`
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
- **CE-150 / CE-158 ROM windows** (`0x8000-0xBFFF` on the LH5803 side,
  `Y2` on the PC-1500 side) — open bus, no module. `PC1500Memory.hpp:35`,
  `LH5803SharedMemory.hpp:30`
- **Cassette (CMT)** — connector pins exist, no FSK / audio path.
  `SystemBus.hpp:27`
- **Buzzer / piezo / sound** — out of scope on both machines.
  `PC1500Memory.hpp:52`

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

`Core/PC1500/PresetFile.cpp`, `Core/PC1600/PC1600PresetLoader.cpp`

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
