# Debugger (VS Code, Debug Adapter Protocol)

Calc-U-1600 contains a debug server. VS Code attaches to it and can debug machine code running in the emulator:
- LH5801 on the PC-1500 / 1500A
- Z80 (SC7852) and LH5803 on the PC-1600

It supports:
- breakpoints, including conditions, hit counts and logpoints
- data breakpoints on memory
- stepping by instruction or by source line, including step over and step out
- registers, memory and disassembly
- the original `.asm` source, when a listing from the assembly run is at hand

There are two ways to use it:
- **Debug your own programs:** build in VS Code, then load the program into the emulator (Build & Load).
- **ROM research:** attach with no program of your own. For example, reset and step through the reset routine from its first instruction.

## How it works

**Transport.** The server lives in the app and listens on 127.0.0.1 only. It accepts one client at a time; a second client is told the debugger is busy. To turn it on:
- tick **Settings ▸ Debugger ▸ Accept a debugger** (port 4711 by default); the dialog shows its live status;
- or start the app with `--dap <port>`, which enables it for that run only and leaves Settings unchanged.

To detach, stop the session in VS Code (Shift+F5), or press **Disconnect** in Settings ▸ Debugger. A second debug session started while one is attached is refused with a message saying this.

The VS Code side is a small extension, `vscode/calcu1600-debug/`. It connects to the server and adds the Build & Load and reset commands.

**Threads.** Each CPU is a thread: `LH5801` on the PC-1500, `Z80 (SC7852)` and `LH5803` on the PC-1600. On the PC-1600, the thread that currently owns the bus is marked `[bus]`. A stop always stops both CPUs, and names the CPU that caused it.

**Call stack = instruction history.** The call stack shows:
- **Frame 0:** the live state, at the next instruction.
- **Frames 1–20:** the last 20 instructions the CPU executed, newest first, e.g. `after C0EE  call KEYGET`. Each shows its registers *after* it ran, and its source line if it has one.

Every CPU records this history all the time, so it is there even when you attach after something went wrong. An interrupt shows up as `interrupt at …`. The history does not use the TRACE file; the two work independently.

**Pausing.** While a debugger is attached, every emulation frame goes through its run control:
- **Paused:** the machine doesn't move, and the title bar shows *Paused (debugger)*.
- **Running:** the machine runs at normal speed, with breakpoints and watches armed.
- **Stepping:** a long step (step over a slow ROM call) runs in frame-sized pieces, so the window stays responsive.

Breakpoints are only armed while the debugger itself runs the machine. A preset load, a program load or a reset's boot run never stops on one.

**Stepping.**
- *Instruction* granularity (the disassembly view) executes one instruction of the chosen CPU.
- *Line* granularity (in source) runs until that CPU reaches another source line, or code that has no source.
- **Step over** a call (`SJP`, `VEJ`, `VMJ`, `CALL`, `RST`, and the conditional forms) runs until the stack is back at its depth before the call. This also works for a `VMJ` whose inline parameters move the return address.
- **Step out** runs until a return (`RTN`, `RTI`, `RET` …) pops the current frame.

**Breakpoints.**
- **Source, function and instruction breakpoints** accept a condition such as `a == 0x10 && [0x7A00] != 0`, a hit condition (`5`, `>= 5`, `% 3`) and a log message (`x={x}`).
- **Data breakpoints** watch memory reads and/or writes. Opcode and operand fetches never trigger them. The machine stops right after the instruction that made the access. Registers can't be watched; use a conditional breakpoint instead.
- **Expressions** (conditions, the watch window, hovers, the debug console, register edits) accept:
  - registers and flags (`cf`, `zf` …) and symbols from the listings
  - `[addr]` (byte), `w[addr]` (word, in the CPU's byte order) and `#[addr]` (a byte from the LH580x's ME1)
  - C operators

**Memory.** The memory view reads without side effects. Device registers that can't be read without disturbing them are shown as unreadable, for example the UART on the PC-1600's LH5803 side and a card's I/O window. Memory references are plain numbers, because VS Code's disassembly view reads them as numbers. The main CPU's addresses appear as they are (`0x40C5`). The PC-1600's LH5803 carries its thread number above the 16-bit address (`0x24200`), and an LH580x's ME1 adds `0x100000` (`0x12F00B`).

**Where to look in VS Code.**
- **Call Stack:** each CPU with its live frame and history. Click a frame to select it.
- **Variables ▸ Registers:** the selected frame's registers, plus two expandable entries: *Flags* and, for the live frame, *Banks* (PC-1600 page banks, or PU/PV).
- **Disassembly:** right-click a frame and choose **Open Disassembly View**. It opens by itself where there's no source.

## Using VS Code

1. **Install the extension:** run `tools/install_vscode_extension.sh`, then reload the VS Code window. The script packages a `.vsix` and installs it; a symlink into `~/.vscode/extensions` doesn't work with current VS Code.
2. **Add the build tasks** from that README to `.vscode/tasks.json`:
   - `sdas: build current file` runs sdaslh5801 → sdld → makebin;
   - `zasm: build current file` runs zasm.

   Their problem matchers put assembler errors in the Problems view. Set `CALCU_SDCC_BIN` / `CALCU_ZASM` if the assemblers aren't in the default checkouts.
3. **Add a launch configuration** (*Add Configuration… ▸ Calc-U-1600: …*). The PC-1500 example:
   ```jsonc
   {
     "type": "calcu1600", "request": "attach", "name": "PC-1500: memtest", "port": 4711,
     "preLaunchTask": "sdas: build current file", "buildTask": "sdas: build current file",
     "preset": "${workspaceFolder}/examples/startup/default-pc1500.pc1500",
     "program": {
       "bin": "${fileDirname}/${fileBasenameNoExtension}.bin",
       "listing": "${fileDirname}/${fileBasenameNoExtension}.rst",
       "address": "0x40C5", "after": "stopOnEntry"
     }
   }
   ```
4. **Start debugging** (F5). The session runs through these steps:
   1. The task builds the program.
   2. **Clean start:** the preset sets the machine up. Without a `preset` in the configuration, the model's default preset from Settings is used; without that, All Reset and a boot to the prompt.
   3. The program is loaded directly, without the Load Machine Code dialog or its advice popup.
   4. Its `CALL` is typed and entered through the app's inbound typing API. The GUI's Paste Text never presses ENTER; this API does.
   5. VS Code stops at the program's first line.
5. **After an edit, press Build & Load** (`⌘⌥L` / `Ctrl+Alt+L`). It rebuilds, does the same clean start, then loads the new binary and listing and starts it, without detaching. Breakpoints move with the code. Set `"cleanStart": false` in `program` to reload in place instead.

The attach settings:

| Setting | Meaning |
|---|---|
| `port` | the server's port |
| `preset` | a preset applied first; it rebuilds the machine |
| `reset` | `none`, `reset` or `allReset`: reset without the boot run |
| `stopOnEntry` | stop right after attaching; after a reset, that is before the first instruction |
| `program` | Build & Load: `bin`, `listing`, `source`, `symbols`, `cpu` (`lh5801` / `z80` / `lh5803`), `address`, `slot` (`S0`–`S2`), `entry`, `after` (`none` / `call` / `stopOnEntry`), `cleanStart` (default `true`), and the bank qualifiers below |
| `listings` | static listings, e.g. of ROM code: `{path, source, cpu, bank, me, pu, pv}` |
| `symbols` | `.SYMBOLS:` tables |
| `buildTask` | the task Build & Load runs |

**ROM research.** Attach with `"reset": "reset", "stopOnEntry": true` to stop on the first instruction after reset:
- PC-1500: E000, the reset vector's target;
- PC-1600: 0000 on the Z80. The LH5803 thread shows its own state.

**Reset & Stop** and **All Reset & Stop** (command palette) and the debug toolbar's *Restart* do the same during a session.

## Listings

**Supported formats.** The debugger reads the listings of the two supported assemblers:
- **sdas:** sdaslh5801 / sdasz80 `.lst`, and the `.rst` that sdld writes with the linked addresses. Prefer the `.rst` for relocatable code.
- **zasm:** `.lst` from `zasm -uwy`.
- **Symbols:** `.SYMBOLS:` tables (`HHHH name` per line).

Neither assembler names the file an included line comes from. The parser therefore walks the listing against the source files themselves and places every line by its text; keep the sources next to the listing. Labels are taken from the source column, because the sdas `.sym` table cuts names to eight characters.

**Loaded and static listings.**
- **Loaded:** a listing that comes with a `program` belongs to exactly the loaded address range. A newer load replaces it. That is how the debugger knows which listing matches which bytes.
- **Static:** listings under `listings` apply in attach order, below any loaded ones.

**Checked against memory.** Every listing is compared with memory when it is bound. One whose bytes don't match is marked stale: it's reported, and it isn't used for source. That covers the wrong build, and the wrong ROM version (an A03 listing on an A04 ROM). A frame whose bytes no longer match its line (self-modifying or overwritten code) is shown as disassembly instead.

**Bank qualifiers** bind a listing, and the breakpoints in it, to a memory state:

| Qualifier | Meaning |
|---|---|
| `bank` | PC-1600 Z80: the page bank 0–7 selected for the address |
| `me` | LH580x: ME0 or ME1. Code always runs from ME0, so `me: 1` is for data. |
| `pu`, `pv` | LH580x: the PU / PV flip-flops, for code in banked ROMs |

A qualified breakpoint only stops while its qualifier holds.

**Later.** Other listing formats (for example the TASM-format PC-1500 ROM disassembly, or library files) can be added as further parsers in `Core/Debug/Listing/`. Nothing else has to change.

## Testing

- **Unit tests:** CoreTests covers the disassemblers (against the assembler's own opcode table and the CPU cores), the listing parsers (real assembler output under `Core/tests/fixtures/listings/`), the source map, breakpoints, expressions and run control.
- **End to end:** `uv run tools/dap_smoke.py --app build/Qt6/Calc-U-1600.app/Contents/MacOS/Calc-U-1600` starts the app with `--dap`, runs a plain session, Build & Load sessions on a PC-1500 (memtest) and a PC-1600 (the ROM dumper), and a ROM reset-and-step session over DAP, then quits the app.

## Known limitations

- The PC-1600's vertical banks (Port 28H, module banks 1+) are not a bank qualifier.
- ME1 can't be written from the debugger; on both machines it is I/O.
- The LH5803 has no BASIC `CALL`. LH5803 code is loaded with `after: none` and entered from your own Z80 code.
- Step back (reverse debugging) is not supported.
- CLion: not set up yet. Whether current CLion attaches to a TCP DAP server natively, or needs a plugin, is still to be checked.
