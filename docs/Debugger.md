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
- **Function breakpoints** name a symbol. They stop on the CPU whose listing or symbol table defines it, and only while that listing's bank qualifier holds. A program's `symbols` load with the program even without a `listing`.
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
- **Memory:** VS Code has no memory panel. It opens memory in Microsoft's Hex Editor extension (`ms-vscode.hexeditor`), which it offers to install the first time.
  - **Opening it:** while paused, hover over a 16-bit register under *Registers*, or a Watch entry, and click the binary-data icon (**View Binary Data**). A Watch entry can be any address or expression, e.g. `0x7600` or `x+0x10`.
  - **Offsets are relative.** The offset column counts from the address you opened, which only the breadcrumb shows. The row labelled `00000000` is that address, and you can't scroll above it.
  - **Real addresses:** open the view from a Watch entry `0`. Offsets then equal addresses, and **Cmd+G** (Go to offset) jumps anywhere.
  - **Editing:** switch the hex editor to *Replace* mode first (the status-bar toggle, or **Hex Editor: Switch Edit Mode**). The edits reach the machine when you save (**Cmd+S**). An insert or a delete can't be saved: the hex editor then tries to rewrite the whole range and reports *Not supported*. Undo it with **File: Revert File**.
  - **Which CPU:** a Watch entry opens memory as the selected frame's CPU sees it. Select an LH5803 frame first to see the PC-1600's LH5803 side. ME1 can't be opened from a Watch entry.

## Using VS Code

1. **Install the extension:** run `tools/install_vscode_extension.sh`, then reload the VS Code window. The script packages a `.vsix` and installs it; a symlink into `~/.vscode/extensions` doesn't work with current VS Code.
2. **Copy the workspace configuration:** `vscode/workspace/tasks.json` and `launch.json` go into the repository's `.vscode/`, which git ignores. If you already have your own files there, merge them in.
   - **tasks.json** has the build tasks. `sdas: build current file` runs sdaslh5801 → sdld → makebin; `zasm: build current file` runs zasm. Their problem matchers put assembler errors in the Problems view. Set `CALCU_SDCC_BIN` / `CALCU_ZASM` if the assemblers aren't in the default checkouts.
   - **launch.json** has five configurations: *Debug on PC-1600*, *Debug on PC-1500A*, *PC-1500: memtest (stock)*, *ROM: reset and stop* and *Calc-U-1600: attach*.
3. **Debug the `.asm` in focus:** *Debug on PC-1600* and *Debug on PC-1500A* work for any program. With the `.asm` in focus, press F5. The session then:
   - assembles it with the machine's assembler (zasm for the PC-1600, sdaslh5801 for the PC-1500A);
   - does a clean start:
     - *Debug on PC-1600:* a plain PC-1600, without the CE-1600P and without memory modules (`examples/debug/debug-pc1600.pc1600`);
     - *Debug on PC-1500A:* a PC-1500A with a CE-163F (`examples/debug/debug-pc1500a.pc1500a`);
   - loads the program at its `.org`;
   - stops on its first instruction, or on `ENTRY` if the source defines that label (or equate) inside the program.

   The configurations set no `address`. A headerless `.bin` without an `address` loads at the lowest address in its listing, which is the source's `.org`. Neither preset reserves memory for the program, so pick an `.org` that BASIC won't overwrite while you debug (the PC-1500A's &7C01 area, or above a `NEW` of your own). Build & Load assembles the file in focus but reloads the one the session started with, so keep that file in focus.
4. **Add a launch configuration for your own program** (*Add Configuration… ▸ Calc-U-1600: …*). The memtest one:
   ```jsonc
   {
     "type": "calcu1600", "request": "attach", "name": "PC-1500: memtest (stock)", "port": 4711,
     "preLaunchTask": "sdas: build current file", "buildTask": "sdas: build current file",
     "preset": "${workspaceFolder}/examples/memtest_stock_debug.pc1500",
     "program": {
       "bin": "${workspaceFolder}/examples/memtest.bin",
       "listing": "${workspaceFolder}/examples/memtest.rst",
       "address": "0x40C5", "after": "stopOnEntry"
     }
   }
   ```
   The build task assembles the file in focus, so keep `memtest.asm` in focus when pressing F5. The preset gives a stock PC-1500 with the program's bytes reserved (`NEW&417D`). A configuration with a memory module would put BASIC's free memory under &40C5, and memtest would overwrite itself. Build & Load types `CALL &40C5` without `,X`, so set **X** (the pass count) under *Registers* at the entry stop.
5. **Start debugging** (F5). The session runs through these steps:
   1. The task builds the program.
   2. **Clean start:** the preset sets the machine up. Without a `preset` in the configuration, the model's default preset from Settings is used, and refused if it's for another model, as when the app applies it; without one, All Reset and a boot to the prompt. Like the menu's loads and resets, it runs with the frame timer stopped and sets the clock from the host afterwards.
   3. The program is loaded directly, without the Load Machine Code dialog or its advice popup.
   4. Its `CALL` is typed and entered through the app's inbound typing API. The GUI's Paste Text never presses ENTER; this API does.
   5. VS Code stops at the program's first line.
6. **After an edit, press Build & Load** (`⌘⌥L` / `Ctrl+Alt+L`). It rebuilds, does the same clean start, then loads the new binary and listing and starts it, without detaching. Breakpoints move with the code. Set `"cleanStart": false` in `program` to reload in place instead.

The attach settings:

| Setting | Meaning |
|---|---|
| `port` | the server's port |
| `preset` | a preset applied first; it rebuilds the machine |
| `reset` | `none`, `reset` or `allReset`: reset without the boot run |
| `stopOnEntry` | stop right after attaching; after a reset, that is before the first instruction |
| `program` | Build & Load: `bin`, `listing`, `source`, `symbols`, `cpu` (`lh5801` / `z80` / `lh5803`), `address` (for a headerless file; default: the listing's lowest address), `slot` (`S0`–`S2`), `entry` (an address or a symbol of the listing; default: the header's auto-run address, else the listing's `ENTRY` if it lies in the program, else the load address), `after` (`none` / `call` / `stopOnEntry`), `cleanStart` (default `true`), and the bank qualifiers below |
| `listings` | static listings, e.g. of ROM code: `{path, source, cpu, bank, me, pu, pv}` |
| `symbols` | `.SYMBOLS:` tables: a path (main CPU), or `{path, cpu, bank, me, pu, pv}` like `listings` |
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
