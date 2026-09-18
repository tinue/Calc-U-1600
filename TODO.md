# TODO

The living backlog: known bugs, unbuilt features, and pre-release
obligations.

## Known issues

- **Scroll Lock doesn't map to "rsv".** Regressed alongside a Tab-focus
  bug (fixed): clicking a button in the debug panel or the plotter
  paper's Copy/Cut controls gave that widget keyboard focus, so Tab (and
  presumably other keys) got eaten by Qt's focus navigation before
  reaching the keyboard handler. Fixed for Tab (`Qt::NoFocus` on those
  widgets); Scroll Lock's mapping (`PC1500KeyboardMap.cpp`) looks correct
  on inspection, so the cause is likely upstream — worth checking whether
  macOS/Qt even delivers a `Qt::Key_ScrollLock` event for the physical
  key (Mac keyboards have none; an external PC keyboard's key of that
  name may report differently or be intercepted before Qt sees it).
- **Pixels on the dot-matrix display render as squares, not dots.**
- **Minor display timing difference**: real LCD hardware is slower than
  the emulation, so a spurious character can briefly appear while
  scrolling.
- **No way to latch Shift from the host keyboard** (tapping Shift doesn't
  produce a visible latched state in the UI).
- **Cmd-C (Ctrl-C) doesn't work in the debug area** — copy shortcut isn't
  reaching/being handled by the debug panel's text widget.
- **CE-1600P / CE-150 plotter: pen colour can drift out of sync after
  OFF/ON.** Deliberately left as a known limitation. Set a colour
  (`COLOR 2`), power off/on, print again — the plotter draws in the
  wrong colour (typically off by a fixed amount from then on). A plain
  calculator *reset* is fine; only OFF/ON drifts it.

  Root cause: pen colour is firmware-RAM state, not a hardware register,
  and there's no colour-home signal the CPU can read — the real "colour 0
  magnet" is a purely mechanical home detent reached by a blind fixed-
  count turret spin during the firmware's OFF→ON re-init. This emulator's
  `AlpsPlotterMechanism` just increments on every turret click with
  nothing forcing it back to 0, so the init spin doesn't reliably land on
  the firmware's assumed colour 0.

  Fix has to detect the firmware's power-on/init turret spin specifically
  (it does carriage homing *and* turret rotation together, which no
  ordinary `COLOR n`/`LPRINT` does) and force the mechanism to colour 0 at
  its end. Do **not** reintroduce a bare "N turret clicks with pen up ⇒
  home spin" heuristic — it fires mid-`COLOR` command and breaks ordinary
  colour changes (tried and reverted once already).
- **PC-1600 emulator runs ~7% fast in both CPU modes** (MODE 0 Z-80 and
  MODE 1 LH-5803), on top of the correctly-paced idle/HALT timing. Ruled
  out: the "1 WAIT per M-cycle" TRM reading (wrong direction — would make
  it slower, not faster); LCD controller busy timing alone (MODE 1 does
  much less display work but shows a similar error). The ~7% is common to
  both CPU cores despite running different clocks through different code
  paths, which points at something shared — the host pacing loop, or both
  cores' cycle accounting being uniformly a little optimistic. Next
  useful measurement: an LCD-free busy-loop benchmark (compute without
  drawing) in both modes, to separate CPU/pacing error from display
  timing.
- **RAM power-up fill byte may be wrong (0xFF vs 0x00).** The codebase
  currently fills RAM with `0xFF` at construction/reset and documents
  this as "confirmed real hardware behavior"
  (`Core/PC1500/PC1500Memory.cpp:26-29,48-58`,
  `Core/CPU/LH5803/LH5803Memory.cpp:24-28`, generalized to the whole
  PC-1500/1500A/1600 family), with the same default propagated into
  `Core/Connector/CE163FCard.hpp` and the generic YAML memory-card
  format's `powerUpFill` (`Core/Connector/MemoryCardDefinition.hpp:86`),
  and cited in `docs/Memory-Card-Definition-Spec.md` /
  `-Format.md` / `User-Guide.md`. A domain-expert check said real
  hardware actually zeros RAM on startup, not `0xFF` — needs a proper
  hardware verification pass. If `0x00` turns out to be correct, this is
  a coordinated fix across those memory constructors/reset paths, the
  card model, the generic card-definition default, ~a dozen test
  assertions (e.g. `lh5803_tests.cpp:97`, `memory_card_tests.cpp:903`,
  `pc1600_slot_ram_tests.cpp:50`), and the two spec/format docs — not
  just a comment fix. Note in passing: open-bus/unmapped-address reads
  and flash-erase-to-`0xFF` are a different mechanism and would be
  unaffected either way; and `Core/PC1600/PC1600Memory.hpp:372,488`
  already zero-inits the Z-80-side internal RAM bank, so the codebase is
  already inconsistent with itself regardless of which byte is correct.
- **`TIME`/the RTC advances at emulated-CPU rate, not wall-clock** — it
  races ahead when the emulator runs faster than real-time, since the
  clock is seeded once from the host and thereafter advanced only by
  emulated CPU cost, never re-synced. Expected behavior once you think it
  through, but worth knowing when authoring presets: a `T1=TIME…T2=TIME`
  bracket measures emulated CPU work (reproducible at any speed), while a
  preset relying on wall-clock-accurate `TIME$=`/`DATE$=` must run at
  authentic speed for the span that matters.
- **Parameterless `- wait:` in a preset can inflate a program's own
  `TIME` measurement** (~2.3x observed) in the GUI, while `- wait: <n>`
  and no wait agree with each other and with headless runs. Working
  hypothesis: while a BASIC program runs, the LH5803 owns the bus and the
  SC7852's 64 Hz/0.5 s timer accumulators are frozen, so a pure headless
  `FOR/NEXT` sees no timer-ISR overhead; the GUI's parameterless-`wait:`
  handback path appears to let the SC7852 step during the loop, so the
  timer ISR runs every iteration and `TIME` reports the (arguably more
  realistic) larger cost. If so, this is the same "timers freeze under
  LH5803 ownership" class of gap as the item above, not a `wait:`-specific
  bug.

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

## Feature ideas

- What is the "second program memory" for BASIC programs on the PC-1600
  (relevant for ROM modules and battery-backed RAM modules)?
- CE-1600F: remaining peripheral support.
- Research MODE 1 (LH-5803/PC-1500-compat mode): does it genuinely reuse
  the old ROM for things like `PRINT`?
- Load a raw `.bin` (assembly) directly, without a SharpDataExchange
  header. Pop up a dialog when disambiguation is needed: which slot when
  more than one is available, and a start address when the header
  (and thus the address) is missing.
- Emulate the PC-1600F floppy drive.
- Emulate the CE-158.
- Allow screenshotting the display.

## Code cleanup backlog

Smaller, contained refactors — take when the relevant area is next
touched, not proactively:

- `PC1600LhsWindow`/`pc1600LhsWindow()`/`PC1600Bank::lhsRemapRow()` have
  no remaining callers outside their own test — either delete all three
  plus the test, or demote the remap table to a documentation comment.
- `readRomFile` is duplicated three times across test files with three
  different bodies; consolidate into `Core/tests/PresetTestSupport.hpp`.
- `Qt6/app/PresetController.cpp`'s `loadPreset` inlines the PC-1600 case
  and leaves an implicit, unnamed PC-1500 path — extract two symmetric
  private methods and reduce `loadPreset` to peek-model → pick →
  commit-or-report.
- `startPC1600()` blocks app startup on six sequential ROM reads; move
  onto the background queue now that the loader's path accessors are
  safe to call off the main thread.
- Module identity should be read from the slot (`virtual moduleName()`
  on `ExpansionCard`), not reported ad hoc by each preset loader — see
  `TODO(slot-identity)` in `Core/Connector/ExpansionCard.hpp`. Precondition:
  the hardcoded prototype cards (`CE155Card`, `PlainRamCard`,
  `CE1638PlusCard`, `CE163FCard`) get duplicated as `.card.yaml`
  definitions and removed, so `SoftwareDefinedCard` is the only
  implementer.
- `Core/PC1500/PresetFile.{hpp,cpp}` is actually family-agnostic but
  lives in the PC-1500 directory; move to `Core/Preset/` next time it's
  opened.
- Converge `PC1500Memory`'s connector ownership (raw pointer, injected by
  `PC1500Machine`) onto `PC1600Memory`'s pattern (owns its connectors by
  value) next time PC-1500 connector wiring is touched.
- `MemorySlotConnector` and `ExpansionConnector` share ~25 lines of
  copy-pasted dispatch shell; a small base class holding the dispatch
  would remove the duplication.
- `PC1600Memory::attachSlot1(size_t)` has no remaining caller outside
  tests now that the old Bridge/Swift path is gone — drop the overload
  and retarget its test at `PlainRamCard` directly.
