# Expansion connectors: mechanical groundwork (no signal-model changes)

**Status:** implemented 2026-09-26 (a9e56f9, db7af2f, ac4dc0a). The signal work stays open in TODO.md.

## Context

TODO.md § "Expansion connectors: one model on both machines" (as updated in 2c1723b). The end goal is
one properly emulated 60-pin connector per machine that every peripheral goes through. Several hardware
questions are still open, though:
- whether pin 15 or 16 carries PV (Expansion-Connectors.md §2.2b);
- how the PU signal reaches the connector on the PC-1600 while the LH5803 runs, since there's no direct
  path (the LHNMIO / SM schematic net);
- how the CE-1600P decodes bank 4/5 and its I/O ports.

**Scope (user decision):** only the mechanical, software-engineering part. The signals that reach a card
stay bit-for-bit what they are today, including the known shortcuts. None of these change: `PinState`
numbering, the 40-pin contact numbers on the 60-pin plug, the Y/S strobes on `SystemBus`, the LH5803
PU/PV handed through, and `PC1600BusPins`. The contact vocabulary, the PC-1600 drive rules, and merging
the two PC-1600 60-pin paths into one wait for the research. The work below prepares for them so that
step becomes a pin-mapping change, not a restructuring. No user-visible change, and the exclusions stay.

## Work items

### 1. One card-chain shell (`Core/Connector/CardChain.hpp`, new)
- `template<class Card, class Pins> class CardChain`. It handles attach (no duplicates), detach, `chain()`
  and `empty()`. It caches `InhibitSource`s (moved out of `SystemBus`) and exposes `inhibitAsserted()`,
  plus first-responder `read(const Pins&, uint8_t&)` / `write(const Pins&, uint8_t) -> WriteResult`.
- A single-slot mode (a 40-pin plug is a chain of one) makes attach replace, matching today's
  `ExpansionConnector::attach`. `attachedCard()` covers the single-slot users.
- `ExpansionConnector` and `MemorySlotConnector` keep their `decode()`/`remapPins()`/`ioWrite()` and
  their public API unchanged. Only the dispatch/attach code moves onto the shell, which removes the
  ~25 copied lines.
- `SystemBus` (PC-1500 60-pin) keeps its `decode()`/`decodeME1()` with the same pins, and swaps its own
  vector and inhibit bookkeeping for the shell.
- `PC1600SystemBus` keeps `PC1600BusPins`/`PC1600ExpansionCard` for now and moves its chain onto
  `CardChain<PC1600ExpansionCard, PC1600BusPins>`. Its `readRom/readIO/writeIO` stay. The shell's
  `write` must fit that bool-returning interface; a small adapter or overload is enough.
- `CardBase`: move the debug/`moduleName` virtuals from `ExpansionCard` into a base class, so that a later
  60-pin card interface can share them. `ExpansionCard` derives from it, and nothing else changes.

### 2. PC-1500 owns its connectors (settled in TODO)
- `PC1500Memory` owns `ExpansionConnector m_expansionConnector{variant}` and `SystemBus m_systemBus{variant}`
  by value, and gets `expansionConnector()`/`systemBus()` accessors.
- `PC1500Machine` drops its members and the `setExpansionConnector`/`setSystemBus` wiring, and
  `PC1500Machine::expansionConnector()/systemBus()` forward. Drop the `if (m_systemBus)` /
  `if (m_expansionConnector)` null checks in `PC1500Memory.cpp`. An empty chain is already the cheap
  fast path.
- `lh5801_tests.cpp` bare `PC1500Memory` instances attach no card, so they behave the same.

### 3. The host stops knowing card address ranges
- **PC-1500** (`PC1500Memory.cpp`): drop `kCe150IoBase/End`. The 60-pin bus gets first refusal on
  every ME1 access, then the internal LH5811, then the ME0 mirror. This is equivalent today: only the
  CE-150's B008–B00F overlaps `isIoChipAddress`, and the end-of-function `readME1` call already offers
  every other address to the bus. The pins passed stay `decodeME1()`'s.
- **PC-1600** (`LH5803SharedMemory`): replace the typed `Ce150Card*`/`Ce158Card*`, `attachCe150/158`,
  `ce15xAttached`, the fixed CE-158-first order and `isCe158Io` with one
  `CardChain<ExpansionCard, PinState>`. `peripheralPins()` is **kept unchanged** as the known LH5803
  PU/PV shortcut, with a comment that points at the TODO.
  - ME1 order: handoff trigger A038 → UART shadow → internal PIO F000 → chain (terminal when a card
    claims it) → the existing fallbacks (8000–BFFF open bus, else the ME0 alias).
  - The CE-158 blocks (D000–D3FF, DE00–DFFF) don't overlap A038, the UART shadow or F000, so
    behaviour is unchanged. One exception: a CE-158 access its card *doesn't* claim would now fall
    through to the ROM alias instead of returning 0xFF. Check `Ce158Card` claims its whole ranges; if
    not, keep that edge exactly by making the card claim it, not the host.
  - Where the chain lives: `PC1600Memory` owns it next to `m_ce1600pBus` (accessor
    `lh5803PeripheralBus()`). `LH5803SharedMemory` reaches it via `m_shared`, as it already does for
    `uart()`/`subCpu()`. This keeps both PC-1600 60-pin halves in one owner, ready for the later merge,
    and avoids the construction-order issue a machine-owned object would bring.
  - `PC1600Machine::attachCE150/158` attach to that chain. The exclusions in the `detach*Locked()`
    calls stay as they are.
- **Debug peeks without side effects.** `LH5803SharedMemory::debugPeek` (`isCe158Io`) and
  `PC1500Memory::debugPeekME1` (`kCe150Io*`) need "would reading this disturb a card?". Add
  `virtual bool readHasSideEffects(const PinState&) const { return false; }` to `ExpansionCard`, and a
  chain query over it:
  - `Ce150Card`: true for its ME1 LH5810 window.
  - `Ce158Card`: true for its ME1 blocks.

  A small visible difference: on the PC-1500, ME1 B008–B00F shows as unreadable only while a CE-150 is
  attached. Today it shows that way whenever the bus exists, which is always.

### 4. Docs
- `docs/Expansion-Connectors-Plan.md`: commit this plan (memory: plans go into docs/).
- TODO.md section:
  - Tick off the raw-pointer item, the copied dispatch, the typed pointers / `isCe158Io`, and the
    `kCe150Io` host decode.
  - Leave the shortcuts list, "Still open", the CE-1600P research and the one-object merge as the
    remaining, research-blocked part.
  - Note that the PC-1600's 60-pin halves now sit side by side in `PC1600Memory`.
- Update the header comments that describe the old wiring: `LH5803SharedMemory.hpp`,
  `ExpansionConnector.hpp`, `SystemBus.hpp`, `PC1600SystemBus.hpp`, `PC1600Machine.hpp`.

## Commits (on dev-0.6.0, one per step, continue without asking)
0. Plan into `docs/`.
1. CardChain + CardBase; the four connector classes on it.
2. PC1500Memory owns its connectors.
3. Card-agnostic hosts: the LH5803 chain, the PC-1500 ME1 first refusal, the side-effect query.
4. TODO/comments.

## Verification
- `tools/run_tests.sh` passes after every commit with **no test expectation changes**. The only allowed
  test edits: `machine.expansionConnector()/systemBus()` call sites still compile via forwarding, and new
  tests for the chain shell (single-slot replace, inhibit cache, first responder) and for
  `readHasSideEffects`. The memory-card, ce155/1638/163f, pc1600_slot_module, connector, ce150/ce158,
  pc1600_ce150 and ce1600p/f tests pass unchanged.
- The PC-1500 CE-150 demo plot is still 1254 points (`headless/pc1500_cli`). PC-1600 CE-150 LPRINT/TEST
  draws completely (`headless/pc1600_cli`). The CE-158 tests pass.
- The Qt6 app builds (`tools/build_app.sh`). Smoke run: attach and detach the CE-150/CE-158/CE-1600P
  on the PC-1600, and the CE-150/CE-158 plus a memory module on the PC-1500. The debug Dump Mem and ME1
  peek views are unchanged.
