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
- **Minor display timing difference**: real LCD hardware is slower than
  the emulation, so a spurious character can briefly appear while
  scrolling. Controller busy time is now modelled (2026-09-23: busy until
  the 4th 216.7 kHz LCD-clock edge after each write, fitted to a
  real-unit scrolling-PRINT benchmark). Recheck whether the spurious
  character still shows.
- **No way to latch Shift from the host keyboard** (tapping Shift doesn't
  produce a visible latched state in the UI).
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
- Emulate the CE-158.
- Allow saving a diskette or memory module into a preset after it has been
  set up (e.g. formatted / populated in a session), so the preset carries
  that media state.
- Allow viewing a memory module's or diskette's binary file contents in the
  debug area, without having to save them out first. For memory modules
  this means the disk part (the RAM-disk filesystem), not the RAM-extension
  part. For a floppy side, SharpDataExchange 0.2.3's `sde_disk_list` /
  `sde_disk_get` already decode the directory and files (BASIC as a
  listing) from the in-memory image.
- Use SharpDataExchange's disk API once the vendored libsharpdx is
  refreshed to 0.2.3 (`tools/refresh_sharpdx.sh` / `fetch_sharpdx.sh`):
  e.g. a preset step that puts a `.bas` / text file straight onto the
  floppy (`sde_disk_put`), without typing `SAVE` in the emulator.
- Watch an inserted floppy's `.floppy.yaml` for outside changes (e.g.
  `sde put` while the disk is in the drive) and reload or warn, instead
  of overwriting them at the next autosave. Until then the rule is
  "eject first" (`docs/Floppy-Image-Format.md` §8).

## Code cleanup backlog

Smaller, contained refactors — take when the relevant area is next
touched, not proactively:

- `PC1600LhsWindow`/`pc1600LhsWindow()`/`PC1600Bank::lhsRemapRow()` have
  no remaining callers outside their own test — either delete all three
  plus the test, or demote the remap table to a documentation comment.
- `Qt6/app/PresetController.cpp`'s `loadPreset` inlines the PC-1600 case
  and leaves an implicit, unnamed PC-1500 path — extract two symmetric
  private methods and reduce `loadPreset` to peek-model → pick →
  commit-or-report.
- `Core/PC1500/PresetFile.{hpp,cpp}` is actually family-agnostic but
  lives in the PC-1500 directory; move to `Core/Preset/` next time it's
  opened.
- Converge `PC1500Memory`'s connector ownership (raw pointer, injected by
  `PC1500Machine`) onto `PC1600Memory`'s pattern (owns its connectors by
  value) next time PC-1500 connector wiring is touched.
- `MemorySlotConnector` and `ExpansionConnector` share ~25 lines of
  copy-pasted dispatch shell; a small base class holding the dispatch
  would remove the duplication.
