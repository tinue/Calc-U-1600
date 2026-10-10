# Pre-execution registers everywhere (history + trace)

**Status: implemented** (453782e, dev-0.7.0).

## Context

The live debugger (frame 0) shows the registers *before* the instruction at the PC, as every debugger does. The two recorded views do the opposite:
- the debugger history (Call Stack frames 1–20, `HistoryRing`),
- the instruction trace (`TraceRing` → `TRACE.bin`, `read_trace.py`, `pc1500_cli`),

Both record registers *after* the instruction (post-execution). As a result, frame 1 repeats frame 0, and the highlighted source line means "not run yet" in frame 0 but "already ran" in frames 1–20. The fix is one rule everywhere: **a snapshot is the CPU state at an instruction, before it runs.** No information is lost, because post(N) = pre(N+1), which is the next newer frame or the live state.

## Rule, per kind of entry

- **Instruction:** the registers as `step()` finds them, before the fetch. The PC register equals the frame's address.
- **Interrupt entry:** the registers at the interrupted point, before the push and the jump to the vector. Label: `interrupt at XXXX`.
- **Z-80 carried prefix (DD before DD/FD):** the state at the start of that `step()`.
- **DD before ED** (the dropped prefix): the frame keeps the ED instruction at `histPc`, as it does today. Its PC register shows `histPc` (taken from the frame address), and the other registers are the state at the start of the step. This is rare, and the plan accepts it.

## Changes

### 1. History ring: capture at the start of `step()`
- `Core/CPU/HistoryRing.hpp`:
  - The register fields of `LH5801HistoryFrame` / `Z80HistoryFrame` become pre-execution; update the comments.
  - Drop `LH5801HistoryFrame::p` and `Z80HistoryFrame::pcAfter`. Both are now redundant with `pc`.
  - Update the header comment ("frames 1..20: the instruction at <pc>, with the registers it started from").
- `Core/CPU/LH5801/LH5801.cpp`:
  - Split `recordHistory()` into `captureHistory()`, which writes the registers into `m_history.next()`, and `commit()`.
  - Normal path: capture after the breakpoint check and before `fetch8()`. At the end, set `pc` / `len` / `interrupt` and commit.
  - Interrupt path: capture before `serviceInterrupt()`, then commit.
  - A parked step (breakpoint) still writes nothing, which is fine because `next()` without `commit()` is harmless.
- `Core/CPU/SC7852/SC7852.cpp`: the same split. Capture into `*m_historyFrame`:
  - before `fetchOpcode()`, after the breakpoint check;
  - before `serviceInterrupt()` (an interrupt that isn't taken just leaves an uncommitted slot);
  - for the carried-prefix early return, before its fetch, i.e. at the same single capture point at the top.
- `Core/Debug/CpuRegisters.cpp`: in `lhHistoryEntry` / `z80HistoryEntry`, take the `p` / `pc` register value from `f.pc`.
- `Core/Debug/DebugTarget.hpp:61`: change the comment to "before the instruction".

### 2. Trace ring: same rule
- `LH5801` / `SC7852`:
  - When `tf` has a register tier, fill a local `CpuFrame` / `Z80CpuFrame` with the registers at the top of `step()`, at the same point as the history capture.
  - At the end, add `opcode` / `cycles` / `seqno` and push it.
  - Replace `recordTraceFrame(tf, pc, op, cycles)` with a begin/push pair. The cost with tracing off stays a single flag test.
  - Cover all three push sites: LH5801 `:334`, Z-80 `:502` (carried prefix) and Z-80 `:529`.
- `Core/TraceTypes.hpp`: say that the register groups are pre-execution.
- `Core/PC1500/PC1500TraceFile.{hpp,cpp}`: bump `kVersion` to 3, because the meaning of the registers changed while the layout didn't. Update the comment.
- `tools/read_trace.py`: `VERSION = 3`. A v2 file is refused with a clear message, following the pre-1.0 policy of no compatibility path. Update the docstring.
- `tools/pc1500_cli.cpp:268`: no code change, because it prints whatever the frame holds. Update the comment if it states anything about timing.

### 3. DAP labels
- `Qt6/app/debug/DapSession.cpp:680`: drop the `after ` prefix. History frames use the same `XXXX  insn` format as frame 0 and keep `presentationHint: subtle`. `frameRegisters` / `frameContext` need no change.

### 4. Comments and docs
- `Core/CPU/WatchSet.hpp:15`: keep "stops after the instruction completes" and drop "like the history ring". After a data-breakpoint stop, frame 1 is the instruction that made the access, with the registers it started from.
- `docs/Debugger.md:310`:
  - Frames 1–20 are "the last 20 instructions, newest first, e.g. `C0EE  call KEYGET`".
  - Each frame shows the registers *before* the instruction ran, like frame 0. To see what an instruction did, look at the next newer frame (or the live state).
  - Mention that frame 1 after a data breakpoint is the access.
- `docs/background/Decisions.md`: new entry "Snapshots are pre-execution". The live frame, the history and `TRACE.bin` all show the state before the instruction. Post(N) is pre(N+1). Don't switch any of them back to post-execution.
- Leave `docs/background/plans/*` alone (historical).

## Tests to update (and extend)
- `Core/tests/disasm_tests.cpp`:
  - `test_lh5801_history_records_bytes_and_post_registers`: rename it to `…pre_registers` and assert the pre-state values. For example, the `ldi xh,0x78` frame has `x != 0x7800` and `a == 0x42`; the first frame has the reset `a`.
  - `test_z80_history_carried_prefix_and_interrupt`:
    - `ld` frame: `iy` is still 0;
    - `exx` frame: `bc == 0xBBCC`;
    - interrupt frame: PC 0x0008 and IFF1 still set (pre-acknowledge);
    - `pcAfter` checks become `pc` checks.
  - `test_z80_history_drops_prefix_before_ed`: drop the `pcAfter` check and assert `bc == 0x1234` pre-state.
  - New check: after N steps, `history.recent(0)`'s registers differ from the live registers when the last instruction changed one (frame 1 ≠ frame 0).
- `Core/tests/lh5801_tests.cpp:378`: `frames[2].a == 0x03` → `0x02`, and add `frames[0].a` = the initial A.
- `Core/tests/sc7852_tests.cpp`, `pc1600_machine_tests.cpp`, `presetloader_trace_tests.cpp`, `debug_target_tests.cpp`: check them and adjust any register value they assert. Most check only pc, opcode or cpuId.
- `tools/dap_smoke.py`: it asserts 21 frames and a history count; check any name or register expectations.

## Verification
1. `tools/run_tests.sh`: everything green.
2. Build the app and run `uv run tools/dap_smoke.py` against it.
3. Manual check in VS Code (memtest project):
   - stop at a breakpoint and step once;
   - frame 1 shows the stepped instruction, with registers equal to what frame 0 showed before the step;
   - frame 0 has changed.
4. Capture a short `TRACE` from the GUI or a preset `trace:` step (`dev/presets/trace_demo.pc1500a`).
   - `python3 tools/read_trace.py TRACE.bin`: an `LDI A,n` line shows the old A, and the next line shows n.
   - A v2 file is refused.
5. Commit to `dev-0.7.0`.
