# Calc-U-1600 Debugger for VS Code

This extension debugs machine-code programs on the Calc-U-1600 emulator:
- LH5801 on the PC-1500 / 1500A
- Z80 (SC7852) and LH5803 on the PC-1600

The debug adapter runs inside the emulator itself, as a DAP server on 127.0.0.1. The extension only connects to it and adds three commands: **Build & Load**, **Reset & Stop** and **All Reset & Stop**. The debugger itself is described in `docs/Debugger.md`.

## Install

From the repository root:

```sh
tools/install_vscode_extension.sh
```

This packages the extension as `headless/calcu1600-debug.vsix` and installs it with `code --install-extension`. Afterwards reload the VS Code window. Re-run the script whenever the extension changes.

Don't copy or symlink this folder into `~/.vscode/extensions`: current VS Code marks such extensions as removed and never loads them.

In the emulator, turn on **Settings > Debugger > Accept a debugger** (port 4711 by default). Alternatively, start the app with `--dap 4711`.

## Build tasks

Both assemblers are local checkouts. The tasks find them through two environment variables:

| Variable | Default |
|---|---|
| `CALCU_SDCC_BIN` | `~/Development/sharp/sdcc-pc1500/sdcc/bin` (sdaslh5801, sdasz80, sdld, makebin) |
| `CALCU_ZASM` | `~/Development/sharp/zasm/zasm` |

Put these tasks in `.vscode/tasks.json`. Each builds the file open in the editor. `vscode/workspace/` has ready-made `tasks.json` and `launch.json` files to copy into `.vscode/`.

```jsonc
{
  "version": "2.0.0",
  "tasks": [
    {
      // PC-1500: sdaslh5801 -> sdld -> makebin. The source's own .org sets the address.
      "label": "sdas: build current file",
      "type": "shell",
      "options": { "cwd": "${fileDirname}" },
      "command": "B=\"$CALCU_SDCC_BIN\"; [ -n \"$B\" ] || B=\"$HOME/Development/sharp/sdcc-pc1500/sdcc/bin\"; N='${fileBasenameNoExtension}'; \"$B/sdaslh5801\" -plosgff \"$N.asm\" && printf -- '-muwx\\n-i %s\\n%s.rel\\n\\n-e\\n' \"$N\" \"$N\" > \"$N.lnk\" && \"$B/sdld\" -nf \"$N\" && \"$B/makebin\" -p -o 0x$(head -1 \"$N.ihx\" | cut -c4-7) \"$N.ihx\" \"$N.bin\"",
      "problemMatcher": "$calcu1600-sdas",
      "group": "build"
    },
    {
      // PC-1600: zasm writes the .lst and .bin in one go.
      "label": "zasm: build current file",
      "type": "shell",
      "options": { "cwd": "${fileDirname}" },
      "command": "Z=\"$CALCU_ZASM\"; [ -n \"$Z\" ] || Z=\"$HOME/Development/sharp/zasm/zasm\"; echo 'in file ${fileBasename}:'; \"$Z\" -uwy '${fileBasename}' '${fileBasenameNoExtension}.lst' '${fileBasenameNoExtension}.bin'",
      "problemMatcher": "$calcu1600-zasm",
      "group": "build"
    }
  ]
}
```

The zasm problem matcher reads the `in file x.asm:` header that zasm prints before its errors; the task prints one for the main file. VS Code's multi-line matchers pick up the first error of each file section; the terminal shows all of them.

## Launch configurations

*Add Configuration…* offers three snippets:
- a PC-1500 program built with sdas
- a PC-1600 program built with zasm
- ROM research: reset and stop

A typical program configuration (the workspace's *Debug on PC-1500A*):

```jsonc
{
  "type": "calcu1600",
  "request": "attach",
  "name": "Debug on PC-1500A",
  "port": 4711,
  "preLaunchTask": "sdas: build current file",
  "buildTask": "sdas: build current file",
  "preset": "${workspaceFolder}/vscode/presets/debug-pc1500a.pc1500a",
  "program": {
    "bin": "${fileDirname}/${fileBasenameNoExtension}.bin",
    "listing": "${fileDirname}/${fileBasenameNoExtension}.rst",
    "after": "stopOnEntry"
  }
}
```

Without `address`, a headerless `.bin` loads at its listing's lowest address (the `.org`). Without `entry`, it starts at the listing's `ENTRY` if that lies in the program, else at the load address.

What the `program` fields do:
- **`after`:**
  - `stopOnEntry` types the program's `CALL` and stops at its first instruction.
  - `call` just types the `CALL`.
  - `none` only loads the program.
- **`cpu`:** on a PC-1600, `z80` (the default) or `lh5803`.
- **`bank`, `me`, `pu`, `pv`:** qualify the listing. For example, `"pv": 1` makes breakpoints in it stop only while PV is set.

**Build & Load** (`Cmd+Alt+L` / `Ctrl+Alt+L` during a session) runs `buildTask`. It then does a clean start (`preset`, else the model's default preset, else All Reset), loads the new `.bin` and listing, and starts the program, all without detaching. Breakpoints move with the code. `"cleanStart": false` reloads in place.

For ROM code without a program of your own, attach with `"reset": "reset", "stopOnEntry": true`. The machine then halts on the reset vector's first instruction. Add ROM listings under `listings`, each with a bank qualifier if it needs one.

If breakpoints can't be set in your `.asm` files, set `"debug.allowBreakpointsEverywhere": true`, or install a language extension for LH5801 or Z80 assembly.
