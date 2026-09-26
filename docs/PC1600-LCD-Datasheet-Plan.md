# PC-1600 LCD: apply the HD61102/HD61203 datasheet facts

## Context

The reference corpus (`SharpPC1500Reference/PC-1600/PC-1600-Display-HD61202.md`,
commit 22436aa) now has a §9 with facts from the Hitachi 1989 datasheets: the status
byte layout, the read pipeline (output register, dummy read), clocking (the HD61203's
fosc = 2·fφ; CK0 217 kHz is the "215 kHz / FS = GND" external clock, so φ ≈ 108.5 kHz),
the busy-time bound 1/fCLK ≤ T_BUSY ≤ 3/fCLK, and reset/VGG behaviour. The TODO entry
"PC-1600 LCD / sub-CPU timing model → LCD half" asks whether `kBusyClocks = 4` stays
inside that bound.

I compared this with `Core/PC1600/PC1600Display.{hpp,cpp}` and read the ROM (new dumps,
bank 0 and bank 6):

- **Busy-wait** (`817F` in bank 6, `0807` in bank 0): `IN A,(59H) / RLA / IN A,(55H) /
  RRA / AND C0H / JP NZ`. It tests only bit 7 of each chip, so adding DB5/DB4 to the
  status byte is safe.
- **Data reads** (`81E8`): busy-wait, then a discarded `IN A,(C)`, which is the
  datasheet's dummy read. After that come real reads, with a busy-wait before *each*
  one (`8AA2`: `IN E/D/L/H,(C)`). This confirms offset +3 = data read from the ROM.
- **SMBLSET** (`822D`): the set number becomes page 7/6/4. It adds `DSPLPTR` (F05CH)
  and goes through `81F0` (page `B8|n&7`), then `81FC` with A=3FH (column 63), then
  `OUT (56H)`. So B=02H is on **page 4**, not packed into pages 6–7. The emulator already
  does this. It settles the reference's §9.4 "Open".
- **Busy time:** the model holds busy for 3–4 CK0 periods = 13.8–18.5 µs. That is
  ~1.5–2 φ periods, inside the datasheet's 9.2–27.6 µs. The TODO's question is answered:
  216.7 kHz is the HD61203 oscillator input, not fCLK, so the fit does not exceed the
  datasheet maximum.

The goal is to make the model faithful where it can be done **without changing timing**,
because the TODO defers any re-fit until the ~0.65 % BASIC residual is resolved. The
timing-relevant datasheet facts go into TODO as the input for that later re-fit.

## Phase 1 — fidelity changes, no timing change (implement + commit)

All changes are in `Core/PC1600/PC1600Display.{hpp,cpp}` unless noted.

1. **Output-register read pipeline.** Replace the `addressCol - 1` lag in `readIO()`'s
   `dataByte` with a per-`Controller` `uint8_t outputReg`. A read returns `outputReg`,
   then latches `pages[addressCol][addressPage]` and increments the column (datasheet
   Fig. 5, reference §9.3). The ROM's sequence (set, dummy read, reads) gives the same
   results as today. The difference is that a write or page change between reads is now
   handled correctly, and the model matches the datasheet instead of an approximation.
   Rewrite the comment to cite the datasheet and ROM `81E8`.
2. **Status byte DB5 = ON/OFF.** Return `0x20` when `!displayOn` (1 = off). DB4 (RESET)
   stays 0 because no RST line is modelled, and the display is correctly *not* reset on
   power-on (`PC1600Machine::powerOnLocked()` keeps it, matching the VGG note). Say so
   in the comment.
3. **No φ clock while CK0 is off.** `tick()` credits T-states only while
   `m_clockEnabled`. Busy then cannot clear while port 37H bit 4 = 0 (reference §9.7:
   the HD61102s get no clock). This does not change timing while CK0 is on.
4. **CK0 off on reset.** `PC1600Memory::reset()` calls `m_display.setClockEnabled(false)`.
   Port 37H bit 4 is 0 at power-on, and the boot ROM sets it. Display RAM and registers
   stay as they are.
5. **Comments and constants.** In the `kBusyClocks` and `readIO()` comments, say that
   an "edge" is one CK0 (HD61203 oscillator) period, that φ = CK0/2 ≈ 108.5 kHz, and
   that 4 edges ≈ 2 φ cycles, inside the datasheet bound. The value stays a fit. The
   header's "offset 3 = data read" becomes ROM-confirmed (`81E8`/`8AA2`). Status-line
   comment in `PC1600StatusLine.hpp`: ROM `822D` confirms column 63, pages 7/6/4 +
   DSPLPTR.
6. **Tests** (`Core/tests/pc1600_keyboard_display_tests.cpp`):
   - Rework `test_display_read_lags_one_column` into an output-register test. The
     dummy read returns the previous latch. Add a case: write between reads → the
     stale latch is returned.
   - Status byte shows `0x20` while the display is off and `0x00` once on and idle.
   - Busy does not clear while the clock is disabled, and clears 4 edges after it is
     enabled.
   - The existing busy and RMW round-trip tests stay green unchanged.
7. **TODO.md** (commit with any pending user edits in it): in "LCD half", record the
   resolution (in bound, fCLK = 108.5 kHz). Replace the Service-Manual check with the
   remaining re-fit inputs for Phase 2 below.
8. **Decisions.md:** add a short entry. The data-read dummy read and DB5 are datasheet
   behaviour, and `kBusyClocks` is a fit that is in bound: do not "correct" it to
   datasheet min/max.

## Phase 2 — timing (record only, do not implement now)

These change the scrolling-PRINT fit, because the scroll/copy routine at `8A2C`/`8A66`
reads 4 bytes and writes 4 per column. They belong to the re-fit after the residual,
per the TODO's order:
- Busy after data *reads* too (the ROM polls before each read, which is consistent with this).
- Busy phase-locked to φ (end on the 2nd φ edge = 2–4 CK0 periods) instead of 4 CK0 edges.
- Instructions and data writes ignored while busy (datasheet: only Status Read is accepted).
  The ROM always polls, so only user ML that doesn't poll would notice.

## Reference corpus feedback (separate repo, own commit)

`SharpPC1500Reference/PC-1600/PC-1600-Display-HD61202.md`:
- §6.3/§9.4: SMBLSET storage is ROM-confirmed as IC3 column 63, pages 7 (B=00H),
  6 (B=01H) and 4 (B=02H), plus `DSPLPTR` F05CH (bank 6 `822D`, `81F0`, `81FC`). Page 4
  lies on commons X33–X40, so the "X49–X64 only" inference needs a note.
- §3/§9.1/§9.3: offset +3 data read and the dummy read are confirmed from ROM code
  (`81E8`, `8AA2`).
- §9.2: the ROM busy-wait tests bit 7 only (`817F`, `0807`).
- Close the matching TODO items.

## Verification

- Build and run the Core test suite (`ctest` in the existing build dir).
- Headless boot with `headless/pc1600_cli`: check the screen dump and status symbols
  (DEG/RUN etc.) after boot, and the cursor blink over several seconds with no
  neighbouring-column corruption.
- Power off/on and APO with `headless/pc1600_power_probe`: the display comes back
  intact and there is no hang in the busy-wait (checks items 3/4).
- Timing unchanged: run the scrolling-PRINT benchmark and `dampflok.bas` via
  `presetprobe`, before and after. The elapsed time must be identical (Phase 1 must not
  move the fit).
- A quick GUI launch (per the GUI launch/quit rule) to see the boot screen and a `LINE`
  / `POINT` program.
