# Debugger IDE integration by use case

**Status (2026-09-30):** planned; P0 (default port 32168) done.

## Context

The DAP debugger works, but its VS Code set-up assumes one workflow: the user copies `vscode/workspace/{launch,tasks}.json` into the Calc-U-1600 repository's `.vscode/`, and the configurations point at `${workspaceFolder}/vscode/presets/...`. So it only really works inside this repository.

There are four use cases, and each needs a different set-up. The numbers are the ones `docs/Debugger.md` uses:

1. **Developing your own program.** You work in the program's own project and have one specific configuration for it.
2. **A program found online.** You work in a collection folder (e.g. `~/Development/sharp/pc1600/Assembler`) on `.asm` files an agent disassembled from `.bin` files. One generic "build and debug the current file" replaces a launch configuration per program.
3. **Developing a ROM extension** (like the host-drive ROM). The same as 1, but the built `.bin` must be plugged into the machine at power-on, possibly replacing a bundled ROM at the same place. Its code often runs from BASIC commands or during boot, not from `CALL`.
4. **Firmware analysis.** Reset and stop, in any folder.

CLion 2026.1 speaks DAP over TCP and has an attach mode. Its DAP settings are per IDE (Debug Profiles), not per project.

Decisions:
- **Per-project settings** go into an IDE-neutral file in the project: a preset with a `debug:` block. `launch.json` only points at it.
- **Extension ROMs** get into the machine through preset keys, not app command-line options. Every clean start re-reads the preset, so Build & Load needs no app restart.
- **Default port** changes from 4711 to 32168.

## What goes where

### One-time installation (user level, nothing copied into projects)

The **VS Code extension** (`code --install-extension`, per user) carries everything generic:
- **Dynamic debug configurations** (`DebugConfigurationProvider`, `TriggerKind.Dynamic`). They appear in the Run and Debug list in any folder, without a `launch.json`:
  - *Calc-U-1600: Debug current file on PC-1600 (zasm)*
  - *Calc-U-1600: Debug current file on PC-1500A (sdas)*
  - *Calc-U-1600: Reset and stop*: use case 4, on whatever machine the app has open.
- **A task provider** (task type `calcu1600`, `assembler: zasm|sdas`, `file`) in place of the copied `tasks.json`. The problem matchers stay contributed as they are.
- **The default debug presets** (today's `vscode/presets/*`), shipped inside the extension and passed to the app as absolute paths.
- **User settings** (`settings.json`, user scope), in place of the environment variables `CALCU_ZASM` / `CALCU_SDCC_BIN`:
  - `calcu1600.port`;
  - `calcu1600.zasmPath`, `calcu1600.sdccBinPath`;
  - `calcu1600.romListings`, `calcu1600.romSymbols`: static ROM listings and symbol tables added to every session, so use case 4 has ROM symbols in every folder.
- **A scaffolding command,** *Calc-U-1600: Create Debug Project…*. A quick pick offers PC-1600 program, PC-1500A program, PC-1600 bus ROM, PC-1500 bus ROM, PC-1600 S1/S2 module and PC-1500 module. It then writes the per-project files below.

**CLion** (after a spike): a user-level Debug Profile "Calc-U-1600", DAP over TCP on port 32168, attach. The project comes from the neutral project file. CLion has none of the extension's commands (Build & Load, resets). Restarting the session does the same, because attaching already does clean start → load.

### Per project (use cases 1 and 3)

| File | Content | Needed? |
|---|---|---|
| `debug.pc1600` / `debug.pc1500a` (project root) | A normal preset: model, modules, ROM keys, clean-start `keys:`. It also has a `debug:` block with `program` **or** `rom`, `listings`, `symbols`, `entry`, `command`, `stopOnEntry`. It can be opened in the app by hand, too. | yes |
| `.vscode/launch.json` | One configuration: `{type: calcu1600, request: attach, project: "${workspaceFolder}/debug.pc1600", preLaunchTask/buildTask}`. Keys set here override the `debug:` block. | for VS Code |
| `.vscode/tasks.json` | Only when the build is more than "assemble one file" (e.g. a Makefile). Otherwise the contributed `calcu1600` task is enough. | optional |
| `*.card.yaml` (use case 3, slot or PC-1500 memory modules) | A ROM card whose `initial-content` uses `encoding: file` for the built `.bin`. This exists already; the file is read again on each parse. | use case 3 |

Use case 2 needs **nothing** per program. For the current file, *Debug current file* uses the first preset it finds:
1. `<basename>.pc1600|.pc1500a` next to the file;
2. `debug.*` in the workspace root;
3. the extension's default preset.

So a single tricky program can get its own set-up from just a sibling preset. Use cases 1 and 2 differ only in whether a project file exists.

### User vs project level

- **User level:** the extension, its settings (tool paths, port, ROM listings) and the CLion Debug Profile.
- **Project level:** only what describes the project: the project preset, a thin `launch.json` and an optional `tasks.json`.
- Nothing is written into `~/.vscode` by hand. The Calc-U-1600 repository itself uses the dynamic configurations; `vscode/workspace/` goes away.

## Phases (one commit each, on dev-0.6.0)

**P0 — Default port 4711 → 32168.**
- Changed in the app default (`Qt6/app/AppSettings.hpp`), the extension, `tools/dap_smoke.py` and the docs.
- A stored `debug/dapPort` of 4711 stays as it is. That's fine before 1.0: Settings ▸ Debugger shows the value, and the user changes it once.

**P1 — App: project file and `debug:` block.**
- `Core/Preset/PresetFile`: parse a `debug:` block. The preset runner ignores it; only the debugger reads it.
- `DapSession`: new attach argument `project` (a preset path). It sets `preset` and the `debug:` fields; explicit attach keys win.
- `command`: a BASIC line typed through `MachineController::typeCommand()` after the clean start and the load, e.g. `FILES "S3:"` or a `CALL` with arguments.
- Reuses `ProgramLoader` and `planLoad()` as they are.

**P2 — App: ROM extensions.**
- **`host-drive-rom: <path>`** replaces the bundled `PC1600-P1-B7-HOSTDRIVE.bin` (`BundledRoms::attachHostDrive`).
- **`bus-rom:`** is a generic expansion-bus ROM module on both machines. It acts on bus signals only ("cards know only the bus"):
  - PC-1600: 60-pin system bus, page-1 bank n.
  - PC-1500: 60-pin expansion connector, an address window with ME0/ME1 and optional PU/PV banking, the way the CE-150 and CE-158 ROMs sit.
  - An explicit file replaces the bundled ROM of an attached device at the same place, e.g. a modified CE-158 ROM with `interface: ce158`.
  - Before building it, check `docs/Decisions.md` and how the CE-150, CE-158, CE-1600P and host drive decode their ROM selects.
- **`debug.rom` mode:** no program load. The listing binds as static, with its bank / PU / PV qualifier, and the existing stale check reports an old build.
- **Build & Load in ROM mode:** build → clean start (the preset is applied again; the ROM is found at power-on) → listings bound again → `command` / `stopOnEntry`.
- **Stop during boot** (`debug.boot: debug`): apply only the preset's hardware part, reset, stop before the first instruction, and run the boot under `DebugController::runSlice()` with breakpoints armed. The preset's `keys:` steps are skipped in this mode. This keeps the rule that preset and boot loops never park on a breakpoint.

**P3 — Extension, generic part (user level).**
- Adds the dynamic configurations, the task provider, the settings and the bundled presets.
- `resolveDebugConfiguration` fills in the current file, its preset (sibling → root → default) and the ROM listings from the settings.
- No repository-specific paths remain.

**P4 — Extension: scaffolding.**
- *Create Debug Project…* writes from templates under `vscode/calcu1600-debug/templates/`:
  - a preset with a `debug:` block;
  - `launch.json`;
  - a hello-world `.asm` per CPU;
  - for the ROM targets, a `.card.yaml` or a `bus-rom` preset.
- It never overwrites a file. It merges into an existing `launch.json`.

**P5 — CLion spike.**
- Check in CLion 2026.1:
  - attaching to an already running TCP server;
  - breakpoints in `.asm` files;
  - whether the attach JSON can name the project directory (else one Debug Profile per project, or an absolute `project`);
  - a build before launch through an External Tool.
- Document the outcome.

**P6 — Docs.**
- Rewrite `docs/Debugger.md` from scratch for users only; internals go to `docs/Debugger-Handoff.md`. The structure:
  1. one-time installation;
  2. setup;
  3. use cases 1–4;
  4. reference: views, breakpoints and expressions, keys, limitations.
- Add Mermaid diagrams where they help:
  - the pieces;
  - the F5 / Build & Load sequence;
  - the preset lookup;
  - bus ROMs and `boot: debug`.
- Add to `docs/Decisions.md`: no copied workspace templates; ROM injection via presets, not the command line.
- Delete `vscode/workspace/` and move `vscode/presets/` into the extension.

## Verification

- **CoreTests:**
  - parsing the `debug:` block;
  - the `host-drive-rom:` override;
  - `bus-rom:` on both machines.
- **`tools/dap_smoke.py`:**
  - attach with `project` (PC-1600 program);
  - a PV-banked PC-1500 `bus-rom:` test ROM, whose breakpoint is hit via `command`;
  - the host-drive ROM override, stopping in bank 7 on `FILES "S3:"`;
  - `boot: debug`, hitting a breakpoint in a ROM's power-on code.
- **Manual, VS Code:**
  - an empty folder shows *Reset and stop*;
  - `~/Development/sharp/pc1600/Assembler` runs *Debug current file* and picks up a sibling preset;
  - *Create Debug Project…* for a program and for a bus ROM, then F5 and Build & Load;
  - the Calc-U-1600 repository works without copied files.
- **Manual, CLion:** the P5 checklist.
