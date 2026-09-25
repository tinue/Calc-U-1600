# CE-1600F floppy — FORMAT self-test handoff

*Written 2026-09-18. **Status: RESOLVED** (same day) -- see §0. §§1-5 are
the original investigation, kept for history; §2's "register reuse"
conclusion and §5's options are superseded by §0.*

## 0. Resolution

`INIT"X:"` now formats a blank disk, and SAVE / LOAD / FILES / DSKF work
on it (verified headless: format → `SAVE"X:HELLO"` → `NEW` →
`LOAD"X:HELLO"` → `LIST`; `DSKF"X:"` = 61952). The problem was the card's
register model, not ROM quirks. Bank-5 Z80 addresses below.

**What 0x492d really does.** It's the seek-with-verify primitive used by
the generic sector read/write paths (callers 0x4759, 0x47c7, 0x4a17). Its
twin 0x4931 is seek-without-verify, and FORMAT uses that one (0x4598).
The `c` tested at 0x4973 is *not* a leftover port address: `pop bc` at
0x4972 restores the value pushed at 0x4933, i.e. the entry flag (0xFF =
verify, 0x00 = no verify). So `inc c; ret nz` just means "return here
unless verifying". On the verify path:

- command 0x80 is **READ ID**. It streams the ID field of the next sector
  (track, sector, size code), the same three bytes FORMAT writes per sector
  at 0x4569-0x4583 (`h`, `l`, `d=1`);
- `pop af` at 0x49a7 gets back the pushed `bc`, so `a` = the seek target
  track. `cp b` checks it against the first ID byte, i.e. "did the head
  land on the right track?";
- on a match it jumps to 0x4950 with the target/flags pair pushed. The
  next `pop bc` puts the flags byte in `c`, which is never 0xFF after a
  `cp` that matched, so `inc c; ret nz` always returns success.

**The real bugs in `CE1600FCard`:**

1. **Fixed 130 ms busy on every command.** In every transfer loop
   (0x4841 read, 0x48a5 write, 0x455f format, 0x4980 read ID), busy
   (base+2 bit0) must stay *set* while bytes move, with bit1 as the
   per-byte data request. Busy dropping mid-transfer counts as an error
   (0x4881 / 0x49b0). After the last byte it must clear within the
   `sub_44e4` b=1 poll (~9 ms), or the ROM times out, pulses the FDC reset
   (port 0x81 via 0x4375) and fails. A fixed timer can't meet both
   constraints, which explains the "shorter delay is worse" observation.
   Transfers now hold busy until their last byte (20 ms idle timeout for
   abandoned transfers). Seeks take 50 ms + 80 ms per track.
2. **No head position.** The ROM puts the sector (0-7) in base+1 and gets
   the track from a seek (0x20 with the target in DATA; 0x01 = restore to
   track 0). The card indexed sectors 0-127 straight from base+1, so every
   track aliased onto track 0.
3. **Every DATA write went into the disk image**, including the seek
   target byte written at 0x493a.
4. **0xA0 is format *track*.** It takes 8 × 3 ID bytes through DATA; it
   doesn't clear a single sector.

**Two red herrings the original trace missed:**

- **Error 160 is stale.** The boot-time drive scan reads the blank
  disk's boot sector and FAT and leaves 160 in F89B before anything is
  typed. Checking F89B after `INIT` without clearing it first proves
  nothing.
- **`INIT"X:"` prompts "Set diskette for X:" and waits for a key.** The
  repro preset (`type` + `wait`) never pressed Enter, so the format never
  started. A working preset:

  ```yaml
  model: PC-1600
  plotter: ce1600p
  keys:
    - type: INIT"X:"
    - wait: 1
    - key: enter
    - wait: 30
  ```

Whatever is in the second half of the FAT sector after a SAVE is leftover
RAM: the ROM writes the FAT from a 512-byte buffer, and only bytes 0-122
mean anything. The ROM also leaves the FAT copy (sector 2) alone on SAVE.
Both are ROM behavior, not emulator bugs.

## 1. What's implemented and working

- `Core/Connector/CE1600FCard.hpp` — the CE-1600F's register-level hardware
  interface (ports 0x70-0x7F, mirroring drive Y:/X:), modeled after the
  real CE-1600P bank-5 ROM's driver code rather than reimplementing
  FAT/IOCS logic (same philosophy as `CE1600PCard` for the plotter).
- Two-sided media (128KB image, two 64KB sides), a disk-changed latch
  (base+2 bit6) modeled on real FDC DSKCHG behavior, real motor-startup
  (0.5s) and per-command access-time (130ms) delays per the Service
  Manual's own spec sheet, and a "green lamp" (`motorOn()`) the GUI reads.
- GUI: `Qt6/app/FloppyDiskManager.{hpp,cpp}`, a disk-selector combo + side
  toggle + save button + lamp indicator in `ControlBar`, always visible on
  PC-1600 (grayed out when the CE-1600P isn't attached, so the layout never
  jumps).
- Persistence: raw `<name>.floppy.img` (128KB) + `<name>.floppy.yaml`
  sidecar, deliberately not the battery-card splice-into-YAML format (see
  `FloppyDiskManager.hpp`'s own comment for why).
- Preset syntax: `floppy: <name>` / `floppy: <name>,A` / `,B`, requiring
  `plotter: ce1600p` (CE-1600F attaches as a union with CE-1600P — there is
  no separate floppy attach/detach).
- Confirmed bug fixes this session, all committed:
  - Base+2 bit0/bit7 is a **busy** flag (must clear to 0 for "ready"), not
    an "error, inverted" flag as an early draft assumed from a composite
    table that turned out to be colored by another emulator's comments —
    re-derived from the real ROM disassembly and confirmed empirically
    (this fix alone eliminated the original infinite polling loop /
    BASIC ERROR 168).
  - The disk-changed latch (base+2 bit6) must clear on **any** command
    write, not just the step command (0x20) — confirmed via live register
    tracing that DSKINIT's very first command is 0x01, and it polls status
    immediately afterward.
  - Realistic access timing is load-bearing, not cosmetic — a poll right
    after issuing command 0x80 needs to see "busy" still asserted; making
    the delay too short breaks it (tested and reverted, see §4).

All of the above is backed by `Core/tests/ce1600f_tests.cpp` (1116
assertions) and `Core/tests/pc1600_preset_tests.cpp`. Primary-source
citations for the register bit layout: **Systemhandbuch Appendix 6**, via
`~/Development/sharp/SharpPC1500Reference/PC-1600/PC-1600-Peripherals-Hardware.md`
§2.6 ("Port-level command/status registers, 78H-7FH") — this independently
confirms the bit table in `CE1600FCard.hpp`'s own comments.

## 2. The open bug

**Symptom**: `INIT"X:"` on a blank (never-formatted) disk fails with BASIC
ERROR 160 ("no disk"). No sector data is ever actually written (confirmed
by the user comparing against real hardware behavior — zero bytes change).

**Confirmed via live IOCS-dispatch tracing** (method in §3): bare
`INIT"X:"` calls **`FORMAT` (IOCS 83H)** first, then **`DSKINIT` (80H)**.
**`CNCTDRV` (81H) is never called.** This overturned an earlier, wrong
assumption that an elaborate step/verify sequence was a skippable
drive-detection self-test — it is not. It is FORMAT's own low-level
implementation, seeking and verifying tracks as it formats them. It has to
work for `INIT"X:"` to succeed; it cannot be bypassed.

**The specific failing subroutine** — confirmed against the real,
already-dumped ROM (`roms/PC1600-P1-B5-CE1600P-OR-F.bin`, corresponding to
`~/Development/sharp/pc1600/roms/romce1600-2.bin`), disassembled with
labels resolved (see §3 for the exact command). All addresses below are
**local file offsets** into that 16KB image; the live Z80 address is
`0x4000 + offset`.

- **`0x92d`** — a small "step one track" primitive (`fnStepOneWithFlag`).
  Entered via a fixed jump to this exact address (not the `0x931` fallback
  entry a few bytes later, which would be the *forward*-direction variant —
  every caller found so far always uses the reverse/homing entry).
  Writes a caller-supplied byte to the DATA register (base+3), issues the
  step command (`0x20`) to the command register, and polls base+2 for
  completion (uses the confirmed-correct busy-bit semantics from §1).
- **`0x976`** — inside that function, reached only when stepping in the
  *reverse* direction (toward track 0): issues command `0x80` (a "check
  home sensor" query, inferred from context — not documented by name in
  any primary source found so far) and polls base+2 again.
- **`0x986`-`0x9a8`** — after that poll succeeds, reads the DATA register
  **three times** (with a base+2 poll between each) and compares the
  first sample against the third (`cp b; jr nz,...`).
  - **Values equal** → falls through to `0x9ab`: loops back to `0x950`
    (re-enters this same function's own busy-poll/battery-check tail) —
    **not always a dead end**: see below.
  - **Values differ** → jumps to `0x9b4`, which **unconditionally returns
    failure** (`ld a,010h` or `ld a,001h` then implicitly-preserved carry
    set — verified with the properly-labeled disassembly, not guessed).
  - Either poll at `0x983`/`0x99d` seeing base+2 bit0 clear (i.e. "not
    busy" — the confirmed-correct "ready" reading everywhere else in this
    ROM) jumps to `0x9b0`, which **also unconditionally fails**
    (`jp 04881h`, a function that always executes an `scf` with no path
    that clears it before returning — confirmed by full trace, not
    inferred).

**So there is exactly one path that can ever succeed**: values equal on
the 3-sample check, which loops back into the function's own tail
(`0x96d` onward: a short delay, then `inc c; ret nz`). Whether this
returns success or loops back into the home-check *again* depends on
whatever the Z80 register `c` happens to hold at that point — and `c` was
last used a few instructions earlier as a **port address** (not as the
"direction flag" it started the function holding). This is confirmed
empirically (see below), not theorized:

- Live tracing (see §3) of one real `INIT"X:"` run showed **3 calls** to
  `0x92d` total. The first two each hit the home-check once, got an
  "equal" result, looped back once, and then returned successfully
  (matching a `c` value that happened to no longer be `0xFF` on the
  second pass). The third call **retried the home-check 3 times** and
  then hit the unconditional failure at `0x9b0` — with the exact same
  first-poll status byte (`0x83`) as the other two, i.e. nothing about
  our own register model differed at that point.

**Conclusion**: whether this specific ROM subroutine succeeds or fails
depends on incidental Z80 register reuse in 40-year-old firmware — almost
certainly *not* a deliberate design, more likely a subtle bug or
edge-case-dependent behavior in the original code that real hardware's
own timing/mechanical feedback papers over in a way this emulation's
DATA-register model doesn't reproduce. There is no register value we can
return that changes this outcome (see §4) — the failure is purely about
which instructions execute and how many times, not about what data those
instructions read.

## 3. How to reproduce / re-investigate

Headless build (needs extra macOS frameworks the checked-in
`tools/build_pc1600_cli.sh` doesn't pass — a pre-existing, unrelated build
script gap):

```sh
clang++ -std=c++17 -Wall -Wextra -O0 -g \
  Core/CPU/LH5801/LH5801.cpp Core/CPU/SC7852/SC7852.cpp \
  Core/CPU/LH5803/LH5803Memory.cpp Core/CPU/LH5803/LH5803SharedMemory.cpp \
  Core/Preset/PresetFile.cpp Core/PC1500/PC1500Keyboard.cpp Core/PC1500/PC1500TraceFile.cpp \
  Core/PC1600/PC1600Memory.cpp Core/PC1600/PC1600SubCpu.cpp Core/PC1600/TC8576F.cpp \
  Core/PC1600/PC1600Display.cpp Core/PC1600/PC1600Keyboard.cpp Core/PC1600/PC1600Machine.cpp \
  Core/PC1600/PC1600BasicTyper.cpp Core/PC1600/PC1600BasicLoader.cpp \
  Core/PC1600/PC1600ProgramPlacement.cpp Core/PC1600/PC1600MachineImage.cpp \
  Core/PC1600/PC1600PresetLoader.cpp Core/Basic/BasicBinaryImage.cpp Core/Basic/BasicProgramSource.cpp \
  tools/pc1600_cli.cpp Core/Basic/vendor/sharpdx/libsharpdx.a \
  -framework IOKit -framework CoreFoundation -framework Security -framework AppKit \
  -o headless/pc1600_cli_dbg
```

Minimal repro preset:

```yaml
model: PC-1600
plotter: ce1600p
keys:
  - type: INIT"X:"
  - wait: 2
```

Run: `./headless/pc1600_cli_dbg --preset <path>.pc1600`. Peek the BASIC
error register with `machine.debugPeek(0xF89B)` (add a temporary
`std::printf` in `tools/pc1600_cli.cpp` after `applyPC1600Preset()`
succeeds — 160 = "no disk").

**Live CPU/IOCS tracing technique** that produced §2's findings (all
temporary instrumentation, added and then reverted — not in the repo):

1. **Resolve absolute jump targets reliably.** Don't hand-compute Z80
   relative-jump targets from `z80dasm`'s default `$+N` notation — this
   produced at least one wrong conclusion earlier in the investigation.
   Instead regenerate with labels:
   ```sh
   z80dasm -a -l -z -g 0x0000 \
     ~/Development/sharp/pc1600/roms/romce1600-2.bin \
     -o /tmp/romce1600-2_labeled.asm
   ```
   (`-g 0x0000` matters — it must match the origin the checked-in
   `~/Development/sharp/pc1600/disasm/z80/romce1600-2.asm` used, or every
   address is off by the difference.)
2. **Find the real work-area base at runtime.** `SC7852` exposes
   `pc()`/`iy()`/`a()`/`bc()`/`hl()` (see `Core/CPU/SC7852/SC7852.hpp`
   "Register access (debug/tests)"). Add a temporary print in
   `PC1600Machine::step()` (it has `m_sc7852` and `m_bank` in scope) or in
   `PC1600Memory.cpp`'s `readIO`/`writeIO` (which already holds `m_cpu`, a
   `SC7852*`) keyed on address ranges of interest.
3. **Find which IOCS number is dispatched, in what order**: watch for the
   *first* instruction executed after PC re-enters the bank-5 window
   (`pc >= 0x4000 && pc < 0x8000 && m_bank.pageBBank() == 5`) coming from
   *outside* it — that catches the actual entry trampoline addresses (this
   is how `0x400E`=FORMAT-entry and `0x600E`=DSKINIT-entry were found; the
   documented "CALL Bank 5, 4008H" dispatch convention from
   `PC-1600-Memory-Bank-Switching.md` does **not** show up as a literal
   `pc()==0x4008` — `BANKCALL` must resolve the jump-table target
   internally without the CPU's PC ever visibly equaling that address).
4. **Correlate fresh calls vs. internal loops**: watch for `pc()` equaling
   a specific function's entry address (e.g. `0x492d`) with the *previous*
   step's `pc()` different from that address — this distinguishes a real
   `CALL` (fresh invocation, counts toward any outer retry budget) from an
   internal `jr`-based loop back to a label mid-function (invisible to a
   naive "did we call this" check, but exactly what's happening in the
   `0x9ab: jr l0950h` retry).

## 4. What's been tried and ruled out

- **Reinterpreting bit0/bit6 semantics** (multiple iterations) — settled;
  confirmed correct against Systemhandbuch Appendix 6, a primary source.
  Do not re-litigate the bit meanings without new primary-source evidence.
- **Reducing the access-time delay** (`kStepAccessTStates`) to make busy
  clear faster — made things *worse* (9/9 immediate failures at the
  `0x9b0` exit, vs. 1/3 with the real ~130ms delay). The realistic timing
  is necessary; do not remove or shrink it.
- **Making the DATA register return a value that varies between separate
  home-check attempts** (while staying stable across the 3 samples within
  one attempt, to avoid the guaranteed-fail "differ" path) — no effect on
  the outcome. Confirms the theory in §2: this is a control-flow/register-
  reuse issue, not a data-content issue. The DATA register's actual
  contents during this specific check do not currently matter to the
  ROM's behavior.
- **Clearing the disk-changed latch only on command 0x20** — wrong (fixed,
  see §1); DSKINIT's real first command is 0x01.

## 5. What to try differently

The user asked to try "a different model" rather than continuing to poke
at this exact mechanism. Some concrete directions, roughly in order of how
much they diverge from the current approach:

1. **Model FORMAT at the IOCS level instead of the register level.**
   Everything else in this emulation (DREAD/DWRITE/etc., per
   `PC-1600-Peripherals-Hardware.md` §2.4) is a thin register-level shim
   under real ROM code — but if this one specific ROM routine has
   irreproducible incidental behavior, consider special-casing FORMAT
   (IOCS 83H) itself: detect the `CALL Bank 5, ...` dispatch for C=0x83
   (now that §3's tracing technique can find the real entry address,
   `0x400E`) and short-circuit straight to "format succeeded, all sectors
   zeroed, valid boot sector written" without going through this specific
   seek/verify dance at all. This breaks the "thin shim over real ROM
   code" philosophy for this one routine but may be the pragmatic answer
   given how fragile the real routine's behavior is.
2. **Model a genuine track-position counter.** Track "current cluster/head
   position" explicitly in `CE1600FCard` (0-15), have the step command
   (`0x20`) move it, and have the `0x80` "check home" query's DATA-register
   response depend on whether position has genuinely reached 0 — this is
   more faithful to what real hardware likely does, but requires reverse-
   engineering exactly what bit pattern signals "home" from the *caller's*
   perspective, which the available primary sources don't document (see
   the now-resolved research task described to the user: IO-Ports.md,
   Peripherals-Hardware.md §2.6, and Work-Area-Map.md were all checked and
   none document a DATA-register "sensor" meaning).
3. **Get a second independent disassembly cross-check.** The sister
   project `~/Development/sharp/pc1600/` is explicitly still at "bootstrap
   — raw auto-disassembly only, no symbols yet" (its own README). A
   proper symbol-annotated pass over just this floppy self-test region
   (functions `0x92d`-`0x9c1` and their callers back through `0x737`)
   might reveal a data-flow detail the ad hoc live-tracing in §3 missed —
   in particular, exactly what the caller at `0x737`/`0x757`/`0x758`
   (outer retry loop, `(iy+011h)` = 3 give-up counter) does with this
   function's return value, and whether there's a *different* call site
   using the `0x931` forward-direction entry that would avoid this branch
   of the function entirely.
4. **Accept the limitation and document it.** Sector-level read/write via
   the raw IOCS registers already round-trips correctly (see
   `test_write_then_read_sector_round_trip` in `ce1600f_tests.cpp`) — only
   the *guided format via `INIT"X:"`* path is affected. A preset or the
   GUI could load a pre-formatted `.floppy.img` (with a valid boot
   sector/FAT already written, matching `PC-1600-Filesystem.md`'s §5.1/§5.2
   layout for format ID `F2H`) to sidestep FORMAT entirely for anyone who
   just wants to use an already-initialized disk. This doesn't fix
   `INIT"X:"` but unblocks everything downstream of it.

## 6. Where things live

- `Core/Connector/CE1600FCard.hpp` — the card itself, extensively
  commented with file-offset citations for every confirmed bit meaning.
- `Core/PC1600/PC1600Machine.{hpp,cpp}` — `attachCE1600P()`'s union
  attach, `ce1600f*()` accessors, `advance()` wiring in `step()`.
- `Core/tests/ce1600f_tests.cpp` — register-protocol tests.
- `Core/tests/pc1600_preset_tests.cpp` — preset-level floppy tests
  (`test_loader_*floppy*`).
- `Qt6/app/FloppyDiskManager.{hpp,cpp}`, `Qt6/app/ControlBar.{hpp,cpp}` —
  GUI.
- `~/Development/sharp/SharpPC1500Reference/PC-1600/PC-1600-Peripherals-Hardware.md`
  §2 (floppy: geometry, IOCS routines, §2.6 port table) — primary-source
  reference, already fully mined for this investigation.
- `~/Development/sharp/pc1600/roms/romce1600-2.bin` — the real, already-
  dumped ROM this whole investigation is built on (same bytes as
  `roms/PC1600-P1-B5-CE1600P-OR-F.bin` in this repo).
- Git history on `dev-0.3.0`: commits `747ccc2`..`e61d41f` cover the whole
  CE-1600F feature; `e61d41f` specifically is the busy-bit/disk-changed-
  latch/timing fix from this investigation.
