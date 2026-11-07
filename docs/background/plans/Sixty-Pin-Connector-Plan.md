# 60-pin / 40-pin connectors: model the real contacts

**Status:** implemented 2026-10-07 (5f8ed70, 6523cff, 6f362ad). Open hardware questions in TODO.md. Verified: plot point hashes of every `examples/plotter` preset identical before/after (`headless/plotcheck`), tape matrices 38/38 (PC-1600) and 26/26 (PC-1500), Qt6 app builds.

## Context

TODO.md § "Expansion connectors: one model on both machines". The groundwork from 2026-09-26
(CardChain, card-agnostic hosts) is done. Several shortcuts remain:

- 60-pin cards (CE-150, CE-158, bus ROMs) read PU/PV from **40-pin** `pin[3]`/`pin[2]`, plus a
  `PinState::me1` flag instead of contact 59. `SystemBus` also drives Y0/Y2/S-block strobes that the
  60-pin plug doesn't carry.
- On the PC-1500's 40-pin plug, the numbers are swapped: measured, contact 2 = PU and 3 = PV.
- The PC-1600 has **two** 60-pin paths. One is `lh5803PeripheralBus()` (PinState, built by hand). The
  other is `PC1600SystemBus`, whose `PC1600BusPins` (ROM offset + bank number + io flag) isn't a pin
  model.

The research that blocked this is now settled in the corpus:
- `Ref/Shared/Expansion-Connectors.md` §2.2b/§4.0: 15 = PU and 16 = PV/PVOUT on both machines;
  14/15/16 = PT/PU/PVOUT carry the page's bank number, MSB first; LH5803 PU is a shared line, and its PV
  goes out through PVIN → PVOUT.
- `PC-1600-Peripherals-Hardware.md` §1.2.2: the CE-1600P's CSNO = MREQ · ELH · PT · /PU · 4000–7FFF,
  with PV picking the 16 KB half.
- TODO "Before fixing": IOE = LH5803 ME1 at xx00–xx0F / 8000–FFFF; IORQ = ME1 and MREQ = ME0 while
  ELH̄ is asserted.

**Goal:** one 60-pin contact model, one `SystemBus` class, **one** connector object per machine (the
PC-1600's driven by both CPUs), and every card decodes real 60-pin contacts.

**User decisions:**
- The CE-158 + CE-1600P exclusion stays. Lifting it is a follow-up that waits for a real-unit check.
- SC7852-side cards ignore cycles while ELH̄ is asserted. For CSNO this is documented. For their I/O
  decode it's an assumption that keeps today's behaviour.

**Rule:** emulated behaviour stays bit-for-bit the same. Where a contact's level is unknown, the drive
leaves it inactive, says so in a comment, and lists it in TODO.

## Design

### 60-pin contact model — `Core/Connector/SystemBusCard.hpp` (new)
- `namespace Contact60` holds contact numbers under each host's signal name. Two names share a number
  where the hosts differ:
  - `kM1 = 10`, `kPT = 14`, `kPU = 15`, `kPV = 16` (PVOUT), `kInh = 25`, `kIorq = 26`, `kMreq = 49`
  - `kDme0 = 56`; `kDme1 = 58` / `kElh = 58`; `kMe1 = 59` / `kIoe = 59`
- `struct SystemBusPins { uint16_t address; bool forWrite; bool pin[61]; }`
  - `true` = asserted for strobes (ELH̄ asserted = LH5803 owns the bus), and the plain level for
    PT/PU/PV. Same convention as `PinState`.
  - The address, data and RD/WR stay in `address`/`forWrite`, as in `PinState`.
- `class SystemBusCard : public CardBase` has `respondsToRead`/`respondsToWrite` (WriteResult),
  `readHasSideEffects`, `cmtOut`/`cmtIn`, `advanceCassette` (LH5801 cycles) and `tick` (SC7852
  T-states).
  - Both time hooks are kept as today, so each host keeps calling only the one it calls now. A CE-150
    on a PC-1600 must not start running its recorder.

### One connector class — `Core/Connector/SystemBus.hpp` (rewritten)
- Holds `CardChain<SystemBusCard, SystemBusPins>` with attach/attachFirst/detach/chain,
  `read(pins, v)`, `write(pins, v)`, `readHasSideEffects`, `inhibitAsserted`, `setCmtOut`/`cmtIn`,
  `advanceCassette` and `tick`.
- It does no host decode, so the `PC1500Variant` member goes.
- The host builds the pins with its own drive functions:
  - **PC-1500** (`PC1500SignalDecode::systemBusPins(addr, forWrite, me1, pu, pv)`): PU→15, PV→16.
    ME0 cycles assert DME0 (56). ME1 cycles assert ME1 (59) + DME1 (58). No Y/S strobes.
  - **PC-1600** (`Core/PC1600/PC1600BusDrive.hpp`, new, pure functions):
    - `lh5803Pins(addr, forWrite, me1, pu, pv)`: ELH̄ (58) asserted; PU→15, PV→16.
      - ME0: MREQ (49) + DME0 (56). DME0 is an assumption: the TRM names contact 56 DME0, the same as
        on the PC-1500.
      - ME1: IORQ (26), plus IOE (59) when `(addr & 0xF0) == 0 || addr >= 0x8000`.
    - `z80MemPins(addr, forWrite, bank)`: MREQ; PT/PU/PV = bank b2/b1/b0; ELH̄ not asserted.
    - `z80IoPins(port, forWrite)`: IORQ only.
    - Not driven, with a comment and a TODO entry each: M1 on memory cycles, PT/PU/PV on I/O cycles,
      DME0 on Z-80 cycles.

### Cards decode contacts (`Core/Connector/…`)
- **PC-1500 family:**
  - `Ce150Card`: ROM = read · DME0 · /PV · A000–BFFF; LH5810 = ME1/IOE (59) · B00x.
  - `Ce158Card`: ROM = DME0 · PV, banked on PU (15); registers on 59.
  - `BusRomCard` (me1 → contact 59, else DME0; PV/PU from 16/15).
  - Z-80 cycles never assert DME0 or 59, so these cards stay silent there, as today.
- **SC7852 family:**
  - `CE1600PCard`: ROM = MREQ · /ELH̄ · PT · /PU · 4000–7FFF, half from PV. I/O 80–83H = IORQ · /ELH̄.
  - `CE1600FCard`: 70–7FH (+81H) on IORQ · /ELH̄.
  - `PC1600HostDriveCard`: bank 7 = PT·PU·PV, ports 90/91H, all with /ELH̄.
  - `PC1600BusRomCard`: bank bits + MREQ · /ELH̄ · 4000–7FFF.
  - These read the real address (4000H+) instead of an offset.
- All of these derive from `SystemBusCard`. Delete `PC1600SystemBus.hpp`, `PC1600BusPins`,
  `PC1600ExpansionCard` and `PinState::me1`.

### Hosts
- `PC1500Memory.cpp`: the `m_systemBus.read/write/readME1/writeME1/me1ReadHasSideEffects(addr, pu, pv…)`
  calls become `m_systemBus.read(PC1500SignalDecode::systemBusPins(...))` etc. The call order is
  unchanged.
- `PC1600Memory`: one `SystemBus m_systemBus` + `systemBus()` replace `m_ce1600pBus`/`ce1600pBus()`
  and `m_lh5803PeripheralBus`/`lh5803PeripheralBus()`.
  - Page-B banks 4–7 → `read(z80MemPins(addr, false, bank))`.
  - I/O 70–9FH → `z80IoPins`.
  - CMT/tick follow.
  - Which accesses get offered stays exactly as today: only the ones `resolveConst()` leaves to the
    connector.
- `LH5803SharedMemory`: `peripheralPins` → `PC1600BusDrive::lh5803Pins`; cardRead/cardWrite/debugPeek
  use `m_shared.systemBus()`. The ME1 order and fallbacks are unchanged.
- `PC1600Machine.cpp`:
  - Every attach/detach goes to `m_z80Mem.systemBus()`, and the exclusions stay.
  - `m_systemBusRoms` + `m_lh5803BusRoms` merge into one `std::vector<std::unique_ptr<SystemBusCard>>`.
  - `PresetBusRomLoader.hpp` types follow.

### 40-pin fix (PC-1500/1500A)
- `PC1500SignalDecode::basePinState`: `pin[2] = pu`, `pin[3] = pv`.
- `resolveSignalPin` for PC1500/PC1500A: PU → 2, PV → 3.
- Fix the `PinState` comment table. No `.card.yaml` uses PU/PV (checked), so the card files don't
  change. The PC-1600 slot stays as it is (3 = PU, 2 = VCC, left low).

## Commits (on dev-0.8.0; one per step, keep going without asking)
0. Plan → `docs/background/plans/Sixty-Pin-Connector-Plan.md` + a row in `docs/background/README.md`.
1. PC-1500 40-pin PU/PV contact numbers (decode, `resolveSignalPin`, comments, the affected tests).
2. `SystemBusCard`/`SystemBusPins`/shared `SystemBus`; the PC-1500 drive; CE-150/CE-158/`BusRomCard`
   ported; `PinState::me1` gone. The PC-1600 LH5803 path uses `lh5803Pins` on a `SystemBus` already.
3. PC-1600: one `SystemBus`, Z-80 drive, SC7852 cards ported, `PC1600SystemBus` deleted, machine/
   preset/tests updated.
4. Docs:
   - TODO section: the shortcuts are done. Remaining open items:
     - DME0/M1/PT-PU-PV levels where unknown.
     - Whether the CE-1600P's IO7N/80–83H decode sees LH5803 ME1 (IORQ) cycles. This is the
       prerequisite for CE-158 + CE-1600P, and the Feature-ideas entry points here.
     - The two cassette clocks.
   - Decisions.md: "60-pin cards decode named contacts; PC-1500-family on DME0/ME1(IOE),
     SC7852-family gated by ELH̄"; "DME0 driven for LH5803 ME0 only".
   - Header comments; developer docs that name `PC1600SystemBus`/`ce1600pBus` (grep `docs/`).

## Verification
- `tools/run_tests.sh` passes after every commit. The only test edits allowed:
  - type/accessor renames;
  - PC-1500 40-pin pin numbers (commit 1);
  - new tests in `connector_tests.cpp`:
    - each drive function's contacts (PC-1500 ME0/ME1; LH5803 ME0/ME1 incl. the IOE rule; Z-80 bank
      4/5/7 → PT/PU/PV; Z-80 I/O);
    - CE-1600P/host drive ignore ELH̄ cycles;
    - CE-150/CE-158 ignore Z-80 cycles.
- These tests must pass unchanged in substance: ce150/ce158/ce1600f/host_directory_drive/
  pc1600_preset, plus the memory-card tests.
- PC-1500 CE-150 demo still 1254 points (`headless/pc1500_cli`). PC-1600 CE-150 LPRINT/TEST draws
  completely (`headless/pc1600_cli`). CE-1600P cassette: `dev/tape-matrix` still passes.
- `tools/build_app.sh` builds. Smoke-attach CE-150/CE-158/CE-1600P/host drive on the PC-1600 and
  CE-150/CE-158 on the PC-1500. Debug ME1 peek views unchanged.
