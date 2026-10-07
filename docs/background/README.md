# Background

Why Calc-U-1600 is built the way it is: settled decisions, the plans
features were built from, and handoff notes from investigations. None of
this is needed to use or build the emulator — for that, see the
[main README](../../README.md) and the
[developer documentation](../developer/README.md).

## Decisions

- [Decisions.md](Decisions.md) — things that look wrong, redundant or
  over-complicated at first sight but are deliberate. **Read it before a
  cleanup, simplification or review**, and add an entry when a recurring
  question gets settled.

## Plans

Each plan was written before the work and committed with it. Most start
with a status note on how the implementation differed. New plans go into
`plans/<Topic>-Plan.md`.

| Plan | Written | Status |
|---|---|---|
| [DAP debug server](plans/DAP-Debugger-Plan.md) | 2026-09-25 | Implemented |
| [Debugger restructuring](plans/Debugger-Restructuring-Plan.md) | 2026-09-25 | Implemented (R1–R12) |
| [Debugger IDE integration by use case](plans/Debugger-Use-Cases-Plan.md) | 2026-09-30 | Implemented (P0–P6) |
| [BASIC debugger: how to plan it](plans/BASIC-Debugger-Planning.md) | 2026-09-26 | Recommendation only, no plan or code yet |
| [Code cleanup backlog (low-risk part)](plans/Code-Cleanup-Plan.md) | 2026-09-26 | Implemented; the rest stays in TODO.md |
| [Clean up `examples/`](plans/Examples-Cleanup-Plan.md) | 2026-09-26 | Implemented |
| [Preset review: one naming rule, presets up to date](plans/Preset-Review-Plan.md) | 2026-09-30 | Implemented |
| [Expansion connectors: mechanical groundwork](plans/Expansion-Connectors-Plan.md) | 2026-09-26 | Implemented; signal work done in the 60-pin plan |
| [60-pin / 40-pin connectors: real contacts](plans/Sixty-Pin-Connector-Plan.md) | 2026-10-07 | Implemented; hardware questions open in TODO.md |
| [PC-1600 LCD: HD61102/HD61203 datasheet facts](plans/PC1600-LCD-Datasheet-Plan.md) | 2026-09-26 | Implemented |
| [PC-1600 sub-CPU, TC8576F CPC and RTC/wake-up](plans/PC1600-SubCpu-CPC-Plan.md) | 2026-09-26 | Implemented |
| [PC-1600: type KBII accented characters](plans/KBII-Typing-Plan.md) | 2026-09-27 | Implemented |
| [Loaders follow MODE and program area](plans/Loader-Mode-Plan.md) | 2026-09-27 | Implemented |
| [Loaders: one file classifier from libsharpdx](plans/Loader-File-Kind-Plan.md) | 2026-09-27 | Implemented |
| [PC-1600 host-directory drive](plans/PC1600-Host-Drive-Plan.md) | 2026-09-29 | Implemented |
| [Drag-and-drop loading](plans/Drag-And-Drop-Plan.md) | 2026-09-30 | Implemented |
| [PC-1500 ROM extension demo: RENUM](plans/PC1500-RENUM-ROM-Plan.md) | 2026-10-02 | Implemented |
| [Reading the LCD as text](plans/LCD-Text-Plan.md) | 2026-10-03 | Implemented |
| [Loader matrix: CE-158/sde vs. fast loader](plans/Loader-Matrix-Plan.md) | 2026-10-03 | Implemented, PC-1500 and PC-1600; results in dev/loader-matrix/ |
| [Copy Screen: the LCD as text](plans/Copy-Screen-Text-Plan.md) | 2026-10-03 | Implemented |
| [Pre-execution snapshots: history and trace](plans/Pre-Execution-Snapshots-Plan.md) | 2026-10-04 | Implemented |
| [Cassette CLOAD / CSAVE via WAV files](plans/Cassette-Tape-Plan.md) | 2026-10-04 | Implemented for PC-1500 + CE-150 and PC-1600 MODE 0; MODE 1 `CLOAD` open in TODO.md |
| [Tape as control-bar media](plans/Tape-Bay-Plan.md) | 2026-10-04 | Implemented |

## Handoffs

Notes left at the end of a working session on work that is still open:
where things stand and what to pick up next. A handoff is removed once
its work is resolved.

| Handoff | Status |
|---|---|
| [Debugger](handoffs/Debugger-Handoff.md) | Open: checks in VS Code, CLion |
