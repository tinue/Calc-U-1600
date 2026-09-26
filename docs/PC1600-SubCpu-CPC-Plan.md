# PC-1600 sub-CPU, TC8576F CPC and RTC/wake-up: bring the emulation up to the new research

## Context

Two new documents in SharpPC1500Reference (commits 72a1c13, 462de33, 2026-09-26):
`PC-1600/PC-1600-CPC-TC8576.md` (register-level TC8576AF reference from the Toshiba data
sheet, plus how the ROM programs the chip) and `PC-1600/PC-1600-SubCpu-LU57813P.md`
(Service Manual pin/power/interrupt facts, command set rebuilt from the ROM).

`Core/PC1600/TC8576F.*` and `Core/PC1600/PC1600SubCpu.*` were written before these existed.
They carry `TODO(trace)` markers, guessed semantics ("9x begins an ALARM$/WAKE$ definition",
"one's-complemented within its nibble"), one known wrong polarity, and the RTC timers
(`WAKE$`, `ALARM$`, `ON TIME$`) are accepted and then dropped. The ON key's wake from
power-off is a stand-in (resume from HALT) that the ROM doesn't need.

Goal: make both chips behave as the data sheet, Service Manual and ROM say; replace guesses
and `TODO(trace)` with citations; implement the RTC timers and the real power-off/power-on
path; then update TODO.md.

### Is more research needed? Yes, and most of it is already done (read-only ROM tracing during planning)

These findings are new. They go into the research corpus first (Phase 0):

1. **Power-off command = operand `EAH`** (timer IOCS 20H, P2-B6 `A88E`). The LH-5803 OFF
   routine (rom1500 `E527`→`E540`) writes raw `15H` (= ~`EAH`) to 21H without a CPL, waits
   for XBUSY, then `HLT`s at `E553`. Closes the sub-CPU doc's open item "which command
   switches the system off".
2. **Power-on is a reset with a cause.** Boot (romI-0 `0346`) reads IOCS 15H (operand `A5H`,
   missing from the TRM) and shuffles it into `FA1BH`. The corpus boot notes list its bits,
   but at "F1ABH", which looks like a typo for FA1BH:
   FA1B b0 ALL RESET, b1 internal RESET, b2 external RESET, b4 POWER ON, b5 external POWER ON,
   b6 WAKE$(0), b7 WAKE$(1)/CI. Reverse-mapped, the sub-CPU answer bits are:
   **b7 internal reset, b6 external reset, b5 ALL RESET, b3 power-on (ON key),
   b2 external power-on, b1 wake timer, b0 CI**. An all-zero answer also gives 10H.
   The OFF path (romI-0 `0B66`→`0B8F`) saves SP at `F0DAH` and writes `A5A5A5A5` at `FA08H`.
   On a power-on reset, `07E6` checks that signature and resumes. romI-0 `0D67` sends the
   `FF00H` (WAKE$(0)) or `FF20H` (WAKE$(1)) command string to the key buffer when FA1B
   b6 or b7 is set.
3. **SRIRQ (A2H) semantics.** The ISR (P1-B3 `419F`) ORs the answer with `F07EH` and keeps the
   masked-off pending bits there. That means the sub-CPU clears the pending bits when they
   are read. Bit use: b1 0.5 s (battery check via SRA0, then SRINP → PHONE/CI, APO
   countdown), b0 → `8075H` (probably analog-in/external keyboard), b7 wake → F127 b7,
   b6 alarm 1 → F127 b6 (ON TIME$), b5 alarm 2 → F127 b5 (ALARM$). The boot mask is
   SRMSK|03H.
4. **Timer data layout** (A84B/A881): the write sends the month as one nibble, then day,
   hour, minute and second as BCD pairs (9 nibbles, the same for the clock and all three
   timers). The read returns 9 nibbles for the clock and 7 for the timers (no seconds).
   Writing a timer clears its F127 bit; writing the clock clears all three.
5. **SWPON nibble** = F12BH: b0 WAKE$(1)/CI power-on, b1 wake-timer power-on, b2 wake beep,
   b3 hour signal. SRPON = `69H` + nibble fetch. SWAB/SRAB = `6AH` followed by `80H`+n
   (write) or a fetch (read).
6. **IOCS 0AH/0BH are PASS** (store / clear-if-match, 8 bytes), tested through SRINP bit 2.
   The emulator already had this; the corpus says "possibly".
7. **RS-232C line polarity** (A524: `IN 23H / XOR 01H / AND m / CP m`): PSR b0 (FAULT, not
   inverted) reads 0 when CS is on; b1/b2 (/SLCT, /PE, inverted) read 1 when CD/DR are on.
   **The emulator has b0 backwards.**
8. The LH-5803 reaches the CPC and 33H via the literal ME1 `#(0x0020–0x0033)` form only
   (rom1500 has no `#(0xA02x)` access). This settles the LH5803SharedMemory `TODO(trace)`.

Still unknown and **left inert and documented, not guessed**: operands `9C–9EH`, `9FH`,
`A7H`, `ABH–ADH`, `AFH`, `E5H`, the LH-5803's raw `23H` (DCH), the analog-in/external-keyboard
mode (1EH/24H/16H/1CH/1DH), SWAB's two bits, `/CTS` and `/DSR` wiring, and the **F-pin
tones** (click, alarm, wake beep; frequency and length unmeasured).

## Phase 0: research corpus (SharpPC1500Reference, commit on `main`)

- `PC-1600-SubCpu-LU57813P.md`: add findings 1–6. Fill in the IOCS 15H/20H rows and the
  SRIRQ/SWPON bit tables, and shrink §8 to what is still open.
- `PC-1600-CPC-TC8576.md` §9.5 / `PC-1600-IO-Ports.md` §7.1/§7.3: polarity (7), SRIRQ bit 0,
  the "1 s = b2" bit (not tested by the ROM ISR; keep it, flagged).
- `PC-1600-Work-Area-Map.md` / `PC-1600-Memory-Bank-Switching.md`: F1ABH → FA1BH (check the
  source scan first), plus FA08H signature and F0DAH saved SP.

## Phase 1: TC8576F to the data sheet (`Core/PC1600/TC8576F.{hpp,cpp}`)

Each item cites `PC-1600-CPC-TC8576.md §n` / DS §n. The provenance paragraph and all
`TODO(trace)` markers go.

1. **PSR b5 and b6 split.** XBUSY (b6) is set by the 21H write and cleared by the sub-CPU's
   ACK. BUSY (b5) is the sub-CPU's Z10. The sub-CPU model exposes both edges (Phase 2);
   `psr()` no longer ORs them from one `busy()`.
2. **DSTB delay.** KI reaches the sub-CPU `Td = tSYS·(PR2+2)` after the write (PR7
   prescaler, XCLK 1 228 800 Hz). With the ROM's values that is about 28 µs. The fitted
   1.66 ms total stays the same: the sub-CPU part becomes 1660 µs − Td. No re-fit (that
   stays in TODO).
3. **Parallel command** ops: `B4H` PRIME on, `B5H` one-shot (ends low), `B6H` off and clear
   all flags including XBUSY. PSR b4 = PRIM. Expose `rs232Selected()` for later use; no
   connector mux yet.
4. **CPC reset** (`/RESET` or param-address D5=1): PR0–PR7 kept. IM1=IM2=1, serial command
   0, TxEMP=1, error/RBRK/RxRDY cleared, XBUSY=0, PRIM=0. Today `reset()` zeroes pr[], and
   `test_command_register_reset_bit_clears_file` asserts that; the test gets rewritten.
5. **Interrupt equations** DS §6.5: Tx int needs TxEN·CTS·empty·!TxINTM. Rx int is
   RxEN·[!RxINTM·(RxRDY+RBRK) + !ERINTM·(FE+OE+PE)]. This removes the spurious INT0
   between the boot's `E0H` and the PR5 load. TxRDY depends on TxINTM (§6.3).
6. **Receiver gated by RxEN**: with RxEN=0 the link isn't polled, so bytes wait in the PTY
   instead of latching. ERS also clears RBRK.
7. **Baud from PR7 + PR1:PR0**: baud = XCLK/K/(8·B), with the B=0 (÷4096) and B=1
   (stopped) cases. `kBaudRefHz = 76800` becomes a derived value.
8. **PSR b0 polarity fix** (finding 7): b0 = !CTS; b1 = CD; b2 = DSR. The no-link defaults
   stay "asserted", now in the right polarity.
9. **CI**: the peer's RI goes to the sub-CPU (`setCiLine`), not into the PSR.

Update `Core/tests/tc8576f_tests.cpp`: split BUSY/XBUSY, PRIME ops, reset retention,
INT equations, polarity, RxEN gating, PR7 baud.

## Phase 2: PC1600SubCpu decoded in the chip's own terms (`Core/PC1600/PC1600SubCpu.{hpp,cpp}`)

- `command()` gets the **operand** (`~port value`, the level on R13–R00). One table keyed by
  operand with IOCS name and source, replacing the raw-nibble `switch` and its
  "complemented within the nibble" explanation. Behaviour is the same for every command
  already handled (the raw→operand mapping above checks out for each existing case).
- Parameter buffer (`F0H`+n resets, `80H`+n appends) and result buffer (`90H` fetch), as
  separate buffers, which is what the ROM's write-then-execute / execute-then-fetch shape
  implies.
- **Handshake**: `strobe(operand)` starts at KI. Commands that return data ACK when the
  answer is ready (end of the window). Commands that return nothing ACK on receipt, with
  BUSY held for the window (Service Manual §4-3 type (ii)). `ackPending()`/`busy()` feed
  PSR b6/b5.
- Unknown operands leave the previous answer standing (as today), with a comment naming them.
- Comments: replace the class header's encoding table with a pointer to
  `PC-1600-SubCpu-LU57813P.md §7`, and fix "request 06H"/"5DH bit 1" style references.

## Phase 3: RTC timers and sub-CPU interrupt (the TODO feature item)

- Store the wake, alarm 1 and alarm 2 timers from `94H/96H/98H`; read back with `95H/97H/99H`
  (7 nibbles). Check wildcard encoding (ALARM$ `??`) against the ROM parser (romIII-3
  `6E35`, rom3b `4F59`) before coding the compare.
- **Minute-carry compare** in `tickOneSecond()` sets pending bits b7/b6/b5. The 0.5 s tick
  sets b1; the 1 s tick sets b2. SWMSK/SRMSK (`A0H`/`A1H`) hold the mask.
- **Z7 → INT6 as a level** = (pending & mask) ≠ 0, dropped by the SRIRQ read (`A2H`, clears
  on read). This replaces `latchSubCpuInterruptCause()` and the toggling
  `halfSecondSignal`. Port 32H bit 6 becomes a live level, like bit 0 (`PC1600Memory::intCause()`).
  Check first: find the RAM-disk/file readiness poller (the "ERROR 163 after ~24 retries"
  comment) and confirm it works with clear-on-read. The CE-1601M tests must stay green.
- SRINP (`A3H`): b5 = !CI (from Phase 1), b2 = password set. b3/b0 stay 0 (unknown, noted).
- SWPON/SRPON (`A4H`, `69H`+fetch) and SWAB/SRAB (`6AH`) stored and read back.
- The rest (ON TIME$ → F127 b6, ALARM$ message, BASIC dispatch) is the ROM's own ISR work,
  and the emulator only has to raise the bits.

## Phase 4: real power-off / power-on (`PC1600Machine`, sub-CPU)

- `EAH` arms power-off. When the bus-owning CPU next HALTs (the LH-5803 at `E553`; PCTRL→Q0
  modelled as "owner halted"), the machine goes **off**. CPUs aren't stepped. `runCycles`
  still advances `advanceSharedClocks` (RTC, timers, buzzer idle, CE-1600F), keeping the
  "RTC keeps running" decision.
- **Power-on sources while off:** ON key (cause `08H`), wake-timer match with SWPON b1
  (`02H`), CI with SWPON b0 (`01H`). Power-on runs `resetLocked()` without touching RAM,
  LCD RAM or the sub-CPU state, then sets the cause. The ROM resumes via FA08H/F0DAH, or
  runs the WAKE$ string.
- Remove `m_onWakePending` and the resume-from-HALT stand-in in `setOnKeyPressed()`/`step()`.
  Rewrite the four ON-wake tests in `pc1600_machine_tests.cpp` (`test_on_key_wakes_…`,
  `test_on_press_before_handoff…`, `…does_not_wake_a_later_park`, the LH-5803 variant) as
  power-state tests. `isPoweredOff()` for the GUI; check what the LCD shows while off (ROM
  display-off vs VEE cut).
- Decisions.md entries: "Power-on is a reset with a cause", "SRIRQ clears on read",
  "Receiver ignores the line while RxEN=0", "Type (ii) commands ACK on receipt".

## Phase 5: timing check, docs, TODO.md

- Re-run the timing benchmarks (`headless/beep/bench/timing*.bas` through `presetprobe`)
  to confirm Phases 1–4 didn't move the fit. Check whether a default/ALL RESET timer state
  now leaves F127H=40H after benchmark A, as on the real unit (the residual hypothesis).
  Record the result under the known issue.
- `docs/PC1600-Serial-Port.md`: polarity, RxEN, PRIME.
- **TODO.md**: remove "Check the TC8576F model against Toshiba's datasheet", the PSR/SSR
  polarity item, the sub-CPU protocol-spec idea (done in the corpus) and the RTC-timers
  feature. Add a new item for the **F-pin tones (deferred by the user)** with a measurement
  recipe for the real unit: key click with KEY click on, `ALARM$` 1 s beep, wake beep
  (SWPON b2), hour signal (b3). Record frequency, length and waveform with the calibrated
  iPhone recorder, then drive `PiezoSampler` from the sub-CPU's F output. Narrow the CTS item to "/CTS pin
  wiring unknown". In the LCD/sub-CPU timing item, tick off steps 1–2, and step 3 gets the
  benchmark outcome. Commit TODO.md whole, with the user's own pending edits.
- Update the memories (`project_pc1600_interrupt_model`, `project_pc1600_serial_port`) after
  implementation.

Commit each phase to `dev-0.6.0` and continue without stopping (memory: autonomous phased
implementation). Save this plan as `docs/PC1600-SubCpu-CPC-Plan.md`.

## Verification

- `tools/run_tests.sh`: all suites, especially `tc8576f_tests`, `pc1600_machine_tests`,
  `ce1600f_tests`, `memory_card_tests` (RAM disk), `ce158_tests`, `pc1600_preset_tests`.
- New headless tests on the real ROM: OFF → machine off, clock advances → ON → prompt and
  program intact. `TIME$`/`WAKE$(0)="…;BEEP 1"+CHR$(13)`, `POWER OFF`, run to the minute →
  powers on and the command runs. `ON TIME$ … GOSUB` fires. `ALARM$` message appears.
  `PASS` still works.
- Serial round-trip through the PTY with `SETCOM`/CS gating (polarity fix) using
  `tools/pc1600_uart_probe`.
- GUI: OFF/ON by key and APO (quit via `calcu1600/quit`, `-ApplePersistenceIgnoreState YES`);
  the user checks WAKE$ in the app.
