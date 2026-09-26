# BASIC debugger: how to plan it

**Status (2026-09-26):** a recommendation for planning a DAP debugger for BASIC programs. There is no plan or code yet.

## Why this document exists

The machine-code debugger (`docs/DAP-Debugger-Plan.md`) took about 12k lines across 90 files. After that it needed a restructuring pass, R1–R12 in `docs/Debugger-Restructuring-Plan.md`, which touched another 70 files. The CE-158, for comparison, took about 1.8k lines. The difference wasn't the code, it was how each feature was planned:
- **CE-158:** a leaf device behind interfaces that already existed (peripheral attach, ME1 decoding, `SerialLink`/PTY). Its spec was closed, and the ROM served as a test oracle.
- **Debugger:** cross-cutting. It ran through both CPU step loops, both machines, threading, the load paths, a VS Code extension and three listing formats. The phases went bottom-up, and each one attached itself to the nearest structure. The wrong interfaces only showed up after the code existed. There was also no automatic oracle, so some bugs only appeared in real VS Code use.

The lesson: **make the big design decisions before writing phases**, and **build one thin feature all the way through before adding breadth**.

## 1. Research phase, no code

The hard unknowns are ROM facts, not C++. Collect them in `docs/BASIC-Debugger-Research.md`, and confirm each one with a probe in `headless/`:
- **Statement boundary:** the ROM address where the interpreter starts executing each statement, for each interpreter. There may be three: PC-1500, PC-1600 MODE 0 (Z-80) and PC-1600 MODE 1 (LH5803). The idle loop at $92B3 is known; the "next statement" equivalent is not.
- **Current line:** where the ROM keeps the current line number or line pointer, and the offset of the statement inside the line (several statements can share a line, separated by `:`).
- **Variables:** the layout of fixed variables A–Z, dynamic variables, arrays and strings, and the BCD float format.
- **Call stack:** the GOSUB and FOR/NEXT stacks.
- **Errors and STOP:** where `ERROR n` is raised and where BREAK/STOP land. These become exception breakpoints.

Each fact becomes a Core test against the real ROM, for example "run this program, the reported line is 30, A=5". This is the automatic oracle the machine-code debugger lacked.

## 2. Settle the architecture before phasing

**Main decision: the BASIC debugger is a layer on top of the existing `DebugTarget`, not a sibling next to it.** It uses only the existing API:
- `runMachine`/`stepMachine`;
- a ROM-address breakpoint on the statement boundary, from the existing `BreakpointSet`;
- `peek` to read the line and the variables.

"Stop at line 30" means breaking at the statement boundary and continuing silently unless the line is 30. This leaves the CPU cores, the machine loops and `SyncOperations` untouched. That was the invasive, expensive part last time.

The plan starts with an **interface inventory**: every existing component, and whether it is reused, extended or new. The components:
- `DebugTarget`, `CpuView`, `BreakpointTable`, `RunControl`, `SourceMap`;
- `DapSession`, `SyncOperations`;
- `BasicBinaryImage`, `BasicPointerTable`, `--dump-basic`.

Any "new" or "extended" entry that touches `Core/CPU` or the machine run loops needs a written justification.

The findings from `docs/Debugger-Restructuring-Plan.md` are things to avoid:
- behaviour piggybacked on unrelated flags;
- `if (cpu == …)` branching instead of a view per variant. Here that means one `BasicInterpreterView` per interpreter, holding that interpreter's ROM addresses and memory layout in one table. Hardcoding fixed ROM addresses is accepted (`docs/Decisions.md`);
- the protocol layer knowing machine layout;
- a second load path.

## 3. Decisions for the user, up front

- **Scope:** which interpreters in version 1? PC-1600 MODE 0 only, or also the PC-1500?
- **Mixed mode:** is BASIC debugging a separate debug type in VS Code, or a "BASIC" thread next to the CPU threads in the same session? The second allows stepping from BASIC into a `CALL` and into machine code, but is a bigger design.
- **Source of truth:** is the source the `.bas` file in the editor, or the program in memory? The user can edit it on the calculator. Recommended: memory, decoded with `BasicBinaryImage`, and mapped to the `.bas` by line number.
- **Features in version 1:** line breakpoints, continue, step by statement or by line, variables. Call stack, errors and conditions come later.

## 4. Phases: one thin feature first

- **Phase 0:** interfaces plus one thin feature on one machine, all the way through: a line breakpoint, continue, and variable A shown in VS Code, with a DAP smoke scenario for it.
- **Review checkpoint:** a `/simplify` or design review, allowed to change the interfaces. At this point a restructuring costs a fraction of what R1–R12 cost.
- **Then add breadth:** the other interpreters, the full variable model, stepping, the call stack, exceptions.

## 5. How to run the planning session

Use plan mode on high effort, and hand it three documents: the research doc, `docs/Debugger-Restructuring-Plan.md` (as the list of things to avoid) and `docs/Debugger-Handoff.md`. A prompt along these lines:

> Plan a BASIC-level debugger. First produce the interface inventory and state the layering decision, then the thin first slice, then breadth. Any change under Core/CPU or to the machine run loops needs justification. Every phase has an automatic check: a CoreTest against the ROM or a DAP smoke scenario.

Do the research phase separately and first. It's cheap, it's where most of the real uncertainty is, and its answers decide whether the layered design holds. For example, if the current line isn't cheaply readable at the statement boundary, the design changes.
