# Calc-U-1600 Debugger for VS Code

Debugs machine code on the Calc-U-1600 emulator:
- LH5801 on the PC-1500 / 1500A
- Z80 (SC7852) and LH5803 on the PC-1600

The debug adapter runs inside the emulator, as a DAP server on 127.0.0.1. This extension connects to it and provides:
- **Configurations** that work in any folder, without a `launch.json`: *Debug current file on PC-1600 (zasm)*, *Debug current file on PC-1500A (sdas)* and *Reset and stop*.
- **Builds** with zasm (PC-1600) and sdaslh5801 (PC-1500), including problem matchers. No `tasks.json` is needed.
- **Commands:** *Build & Load* (`Cmd+Alt+L` / `Ctrl+Alt+L`), *Reset & Stop* and *All Reset & Stop*.
- **Settings** for the assembler paths, the port, and ROM listings added to every session.

The user manual is `docs/Debugger.md` in the Calc-U-1600 repository.

## Install

From the repository root:

```sh
tools/install_vscode_extension.sh
```

The script packages `headless/calcu1600-debug.vsix` and installs it with `code --install-extension`. Reload the VS Code window afterwards, and re-run the script whenever the extension changes. Don't copy or symlink this folder into `~/.vscode/extensions`: current VS Code never loads such extensions.

## Settings

| Setting | Default |
|---|---|
| `calcu1600.port` | `32168`, as in the app's Settings ▸ Debugger |
| `calcu1600.zasmPath` | `$CALCU_ZASM`, else `~/Development/sharp/zasm/zasm` |
| `calcu1600.sdccBinPath` | `$CALCU_SDCC_BIN`, else `~/Development/sharp/sdcc-pc1500/sdcc/bin` |
| `calcu1600.romListings` | listings added to every session: a path, or `{path, source, cpu, bank, me, pu, pv}` |
| `calcu1600.romSymbols` | `.SYMBOLS:` tables added to every session: a path, or `{path, cpu, bank, me, pu, pv}` |

## Configuration keys

On top of the app's attach keys (`preset`, `reset`, `stopOnEntry`, `program`, `listings`, `symbols`, `project`, `command`, `boot`; see the manual), the extension understands:

| Key | Meaning |
|---|---|
| `currentFile` | `pc1600` or `pc1500a`: debug the `.asm` in the editor |
| `build` | `{assembler: zasm \| sdas, file}`: assembled before the session and on Build & Load |
| `buildTask` | a task to run instead of `build`, e.g. a Makefile target |
| `port` | the server port, if it isn't `calcu1600.port` |

If breakpoints can't be set in `.asm` files, set `"debug.allowBreakpointsEverywhere": true`, or install a language extension for LH5801 or Z80 assembly.
