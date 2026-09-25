# DAP debug server for Calc-U-1600

> **Status (2026-09-25): implemented** in phases 1–6 (a7b062b … d340fc4). The user documentation is `docs/Debugger.md`. The implementation differs from this plan in these places:
> - **Watches:** each CPU has its own watch set, because the PC-1600's two CPUs have separate address spaces.
> - **Arming:** breakpoints and watches are armed only inside the debugger's frame slices, so a boot, a preset or a load can never park on one.
> - **Run control lives in Core:** `debug::BreakpointTable` and `debug::RunControl` are there and unit-tested; the Qt layer is thin.
> - **Build & Load order:** clean start (preset / default preset / All Reset), then a direct load, then auto-start through `MachineController::typeCommand()`. The GUI paste still never presses ENTER.
> - **Message queueing:** DAP messages wait while the app runs a synchronous load.
> - **Added:** `calcu1600/quit` and the `--dap <port>` option for scripted runs.
> - **Still open:** the manual VS Code check and the CLion setup.

## Context
Machine-code work on the PC-1500 (LH5801) and PC-1600 (Z80 SC7852 + LH5803) is debugged today only by the TRACE file and CLI poking. We want the emulator to offer a **Debug Adapter Protocol** server over TCP. VS Code attaches to it (CLion later) and gets breakpoints (including conditional ones), stepping, registers, memory and disassembly. When a listing from an earlier assembly run exists, VS Code also shows the original `.asm` source.

Two use cases:
- **Main:** debugging our own assembly programs, built in the IDE and pushed onto the emulator (see Build & Load).
- **ROM research:** debugging with no program of our own, e.g. single-stepping the reset routine from its first instruction. Nothing in the debugger may assume a loaded program.

The CPUs are presented as DAP **threads**: one on the PC-1500, two on the PC-1600. An always-on history ring is presented as the **call stack**. Frame 0 is the live state at the next PC. Frames 1..20 are the last 20 executed instructions, each with its post-execution registers.

**Decisions already made with you:**
- **Source view:** show the original `.asm`, falling back to the disassembly view.
- **Listing formats:** sdaslh5801 `.lst`/`.rst`, sdasz80 `.lst`/`.rst`, zasm `.lst`, and `.SYMBOLS:`/`.sym` tables.
- **Session model:** attach, with an optional program or preset load first.
- **v1 features also include:** step over and step out, memory view and edit, register edit, and data breakpoints.
- **Ring semantics:** post-execution registers, with the pre-execution PC (like `recordTraceFrame`, which runs after `execute()`).
- **LH5803:** the same opcodes as the LH5801. One decoder, no variant flag.
- **Data breakpoints:** memory only. Registers get no `dataId`; a register condition is a conditional breakpoint.
- **History:** a small always-on ring per CPU (the Calc-U-59 pattern), separate from the TRACE file ring. The debugger never forces `TRACE_FULL`.
- **Bank qualifiers:** PC-1600 Z80 `bank` 0-7 (vertical banks not supported); LH580x `me` 0/1 (ME0/ME1) plus `pu` and `pv`. See Core item 7.
- **Listings are bound to loads:** the IDE builds, then pushes the `.bin` and its listing through the machine-code loader, so the emulator knows which listing belongs to which loaded range. See Build & Load.

**Current state found by exploration:**
- **No disassembler** exists in the repo.
- **No emulation thread:** the emulator runs on the GUI thread from the `EmulationPacer` 16 ms QTimer, so a QTcpServer on the GUI thread needs no extra locking.
- **Breakpoints only on the LH5801:** `BreakpointSet` in `Core/CPU/TraceRing.hpp` is not used by the GUI, has no skip-once, and `PC1600Machine::runCycles` ignores it.
- **The SC7852 has no breakpoints** and no alternate-register accessors.
- **Missing peeks:** there is no ME1 or LH5803-side peek.
- **No Qt6::Network and no JSON library.** QJsonDocument arrives with Qt6::Core.
- **Trace frames lack operand bytes:** they hold only the opcode or prefix word. The history ring (Core item 1) carries the bytes instead.

Source rule: opcode tables and ISA details cite the Sharp LH5801 TRM, the Zilog Z80 manual, the SC7852 docs and ROM dumps. No other emulator is cited as an authority, and none is called "upstream".

---

## Architecture

```
VS Code ──DAP/TCP 127.0.0.1:port──► Qt6/app/debug/DapServer (QTcpServer, GUI thread)
                                         │  JSON (Content-Length framing, QJsonDocument)
                                         ▼
                                    DapSession ── DebugController (pause state, run modes)
                                         │                 │
                                         ▼                 ▼
                              Core/Debug/* (Qt-free, CoreTests)     EmulationPacer / MachineController
                              ├─ Disasm (LH5801, Z80)
                              ├─ ListingSource parsers → SourceMap (+ load bindings, BankKey)
                              ├─ DebugExpression (conditions, evaluate)
                              └─ DebugTarget (PC1500 / PC1600 impl: threads, regs, peek/poke, history, step)
```

### Core (Qt-free, unit-tested)
1. **Always-on history ring.**
   - `Core/CPU/HistoryRing.hpp`: `HistoryRing<Frame, 32>`, a plain array plus a head counter, no mutex. Writer and reader both run on the emulation thread (the GUI thread). Same pattern as Calc-U-59's `m_frameRing`/`m_frameHead`.
   - New frame types `DebugFrame` (LH580x) and `Z80DebugFrame`, one per retired instruction: pre-execution PC, instruction bytes (`bytes[4]`, `len`), post-execution registers, cycles; on the LH580x also PU and PV.
   - Recorded unconditionally in `LH5801::step()` and `SC7852::step()`, next to `recordTraceFrame`. The bytes come from a per-step buffer filled by `fetch8`/`fetch16`/`fetchOpcode`, which is correct for self-modifying and banked code. An SC7852 split DD/FD prefix step carries its prefix byte into the next frame.
   - Cleared on reset, so history never shows code from before a reset.
   - `TraceRing`, `CpuFrame` and `Z80CpuFrame` stay untouched: the TRACE.bin format and the TRACE file ring are independent of the debugger.
   - Cost is measured: the CE-150 demo and the PC-1600 timing tests must not regress.
2. **Disassemblers:** `Core/Debug/Disasm/LH5801Disassembler.{hpp,cpp}` (the LH5803 uses the same one) and `Z80Disassembler.{hpp,cpp}`.
   - API: `Decoded decode(uint16_t addr, FetchFn)`, returning text, length, a flow kind (`None/Jump/CondJump/Call/Return/Vector`) and a target.
   - The flow kind drives step over and step out. It covers `SJP`/`VEJ`/`VMJ`/`RTN`/`RTI` on the LH5801 and `CALL`/`RST`/`RET*`/`RETI`/`RETN` on the Z80.
   - Undocumented opcodes are included.
   - An optional symbol resolver prints labels.
   - Backward resync for DAP `disassemble` with a negative offset: anchor on listing or trace addresses where known, otherwise back up and pick the decode path that lands on the target.
3. **CPU debug hooks.**
   - **SC7852 breakpoints:** a `BreakpointSet`, checked before fetch when not mid-prefix and gated by `TRACE_BREAKPOINTS`.
   - **Skip-once resume:** a `resumePastBreakpoint()` on both CPU classes, so continue can leave a breakpoint.
   - **SC7852 registers:** alternate-set getters and setters, plus `setI`/`setR`/`setIM`/`setIFF`.
   - **Stop on breakpoint in `PC1600Machine::runCycles`:** today an LH5803 breakpoint would spin as a halt tick. `runCycles` must break on `consumeBreakpointHit()` from either CPU and report which CPU hit.
   - **Side-effect-free peeks:** `PC1500Memory` ME1 and `LH5803SharedMemory` (ME0/ME1) get peeks. The I/O windows read their register latch without clear-on-read effects.
4. **Data breakpoints.**
   - Memory only. A `WatchSet` (address ranges plus read/write kind, with an atomic "any watches" fast path) is checked in each machine's bus wrappers: PC-1500 ME0/ME1, the PC-1600 Z80 memory bus, and the LH5803 bus. On the LH580x each watch carries an ME0/ME1 selector.
   - Read watches ignore opcode and operand fetches: the CPU sets an `m_fetching` flag around its fetch helpers.
   - A hit latches the address and value. The machine stops after the current instruction, consistent with post-execution semantics.
5. **`Core/Debug/DebugTarget.hpp`**, with `PC1500DebugTarget` and `PC1600DebugTarget`.
   - Threads (CPU id and name) and which CPU owns the bus.
   - A register list with get/set by name, including the flag bits.
   - Peek/poke in a CPU's own address space, with an ME0/ME1 selector for the LH580x.
   - History of up to 20 frames per CPU, from the history ring.
   - Bank context for `BankKey`: the PC-1600 page banks from `debugBankState()`, and PU/PV on the LH580x.
   - A one-shot "stop before next instruction" flag, armed before a reset for `stopOnEntry`.
   - `stepInstruction(cpu)`: steps the machine until that CPU has retired one instruction, with a cap. On the PC-1600, stepping the CPU that does not own the bus runs until it does.
   - `runUntil(budget, stopPredicate)`: a per-instruction loop for step over/out and conditional stops.
6. **`Core/Debug/DebugExpression.{hpp,cpp}`**: a small C-like expression parser and evaluator.
   - It knows register names and flags per CPU, memory reads (`[addr]` for a byte, `w[addr]` for a word in CPU endianness), symbols, and hex (`0x`/`$`/`&`) and decimal literals.
   - Operators: `== != < <= > >= && || ! + - & | ^ << >>`.
   - It is used for breakpoint conditions, hit conditions, logpoint `{expr}` interpolation, watch/hover/REPL `evaluate`, and `setVariable` values.
7. **`Core/Debug/Listing/`**: parsers behind one small `ListingSource` interface feed one `SourceMap`.
   - Each parser yields `(cpu, addr, bytes, file, line)` records plus labels. A new format later means one new parser; the SourceMap, bindings, `BankKey`, verification and DAP layer don't change.
   - `SdasListing` reads `.lst`/`.rst` for both sdaslh5801 and sdasz80: address, bytes, the source line-number column and the file. `.rst` is preferred when present because it holds the relocated addresses.
   - `ZasmListing` reads the `.lst` code lines and maps them 1:1 to source lines. The symbol table's `file:line` entries are used to anchor and verify that mapping, and to find include files.
   - `SymbolFile` reads `.SYMBOLS:` tables (`HHHH name` pairs) and sdas `.sym`.
   - `SourceMap` provides:
     - Lookups keyed by `(cpu, addr)`: address → (file, line), and (file, line) → addresses, snapping to the next line that has code.
     - A label ↔ address map.
     - An optional **`BankKey`** per listing: a set of optional fields, each an implicit breakpoint condition evaluated on a raw hit.
       - **PC-1600 Z80 `bank` 0-7:** the bank currently selected for the page containing the address (`pageABank`…`pageDBank` from `PC1600Machine::debugBankState()`). Vertical banks (Port 28H) are not supported.
       - **LH580x `me` 0/1:** ME0 or ME1. Instruction fetch is ME0-only (`LH5801::fetch8` reads `bus.readME0`), so `me:1` applies to data breakpoints, memory references and listings of ME1 data; a code breakpoint with `me:1` comes back `verified:false` with a message.
       - **LH580x `pu` / `pv` 0/1:** the live PU/PV flags, for code in the banked ROMs.
     - **Bindings and precedence:** a listing loaded with a program is bound to the loaded range `[address, address+len)` plus its `BankKey`. A newer load replaces the bindings its range overlaps. Bound listings win over static attach `listings` (typically ROM listings), which apply in attach order.
     - **Byte verification:** after a load, and for static listings at attach, the listing's object bytes are compared with memory through the side-effect-free peek. A mismatch sends an `output` warning and marks the listing stale; its breakpoints become unverified. This catches e.g. an A03 ROM listing attached to an A04 ROM. On a stop, if the bytes at the PC differ from the listing (self-modifying or overwritten code), that frame falls back to the disassembly view.
   - **Later, not planned:** disassembled ROM listings (e.g. Jeff Birt's PC-1500 ROM disassembly, TASM `.lst` format: `LINE ADDR BYTES source`) and external library files, as further `ListingSource` parsers attached as static listings.
   - Before writing each parser, commit one real fixture per format under `Core/tests/fixtures/listings/`, built from `examples/memtest.asm` (sdas), a small Z80 sample (sdasz80) and the pc1600 rom-dumper `.lst` (zasm).

### Qt app
8. **`Qt6/app/debug/DapServer.{hpp,cpp}`**: a QTcpServer bound to **127.0.0.1 only**.
   - One client at a time; a second connection is refused with an error message.
   - Content-Length framing and QJson.
   - Link `Qt6::Network` in `Qt6/CMakeLists.txt`, in both `find_package` and `target_link_libraries`.
9. **`Qt6/app/debug/DapSession.{hpp,cpp}`** handles requests. Capabilities:
   - configurationDone
   - conditional, hit-conditional and log-point breakpoints
   - function breakpoints (by symbol) and instruction breakpoints
   - the disassemble request and stepping granularity
   - read/write memory
   - setVariable
   - data breakpoints
   - evaluate for hovers
   - restart (mapped to a machine reset, see ROM research)

   | Request | Behaviour |
   |---|---|
   | `initialize` / `attach` / `configurationDone` / `disconnect` | Disconnect clears breakpoints and resumes. |
   | `threads` | PC-1500: `LH5801`. PC-1600: `Z80 (SC7852)` and `LH5803`. The name marks which one owns the bus. |
   | `stackTrace` | Frame 0 is the live PC. Frames 1..20 come from the history ring, newest first, named `after C0EE  CALL KEYGET`. Each frame's `source`/`line` comes from the SourceMap when mapped, and `instructionPointerReference` is set on every frame. |
   | `scopes` / `variables` | **Registers**: live for frame 0, post-execution for history frames. It includes a flags child that expands to bits. The Z80 shows the main and alternate sets, I/R/IFF/IM. **Banks** (PC-1600 bank state, PU/PV) is read-only. |
   | `setVariable` | Frame 0 only; the value is parsed by DebugExpression. |
   | `setBreakpoints` / `setFunctionBreakpoints` / `setInstructionBreakpoints` | Source line → address via the SourceMap, snapping to the next code line and returning the `verified` line. `condition`, `hitCondition` and `logMessage` are supported. |
   | `setDataBreakpoints` / `dataBreakpointInfo` | Memory only: `read`, `write` and `readWrite`. Registers return no `dataId`. The data ID is `cpu:space:addr`, where space is `me0`/`me1` on the LH580x. |
   | `continue` / `pause` / `next` / `stepIn` / `stepOut` / `stepBack` | `stepBack` is not supported. `stepIn` and `next` step one instruction when granularity is `instruction` or no source is mapped, and otherwise step until the source line changes. `next` over a Call-kind instruction runs to the return address with SP ≥ the entry SP. `stepOut` runs until a Return-kind instruction retires with SP above the entry SP. |
   | `disassemble` | Uses the thread's CPU and the SourceMap for `location`/`line`, and symbol labels. |
   | `readMemory` / `writeMemory` | `memoryReference` is `cpu:hexaddr`, plus `:me1` for ME1 on the LH580x, using peek/poke with no side effects. |
   | `evaluate` | Watch, hover and REPL, through DebugExpression. |
   | `restart` | Resets the machine per the attach config's `reset` and `stopOnEntry` (same as `calcu1600/reset`). |
   | `calcu1600/load` (custom) | Build & Load during a session; see below. |
   | `calcu1600/reset` (custom) | `{kind: "reset"|"allReset", stop}`; see ROM research. |

   Events:
   - `stopped`, with reason `breakpoint`, `data breakpoint`, `step`, `pause` or `entry`, the threadId of the hitting CPU, `allThreadsStopped: true` and hit breakpoint IDs.
   - `continued` and `output` (for logpoints).
   - `thread` exited/started and `invalidated` on a machine rebuild. The adapter re-applies breakpoints to the new machine.
10. **`Qt6/app/debug/DebugController`**, owned by MachineController.
    - **Paused flag:** `EmulationPacer::onTick` skips `advance()` while paused but still refreshes views.
    - **Run modes:**
      - Free run uses `runCycles` with breakpoints. On a raw hit it evaluates condition, hit count and bank qualifier. If the result is false, it calls skip-once and continues within the same tick's budget.
      - Step, step over, step out and conditional stops use `DebugTarget::runUntil` with a predicate, time-sliced per tick so the GUI stays live.
    - **Trace flags:** the controller ORs only `TRACE_BREAKPOINTS` into the flags that `MachineController` computes, and only while at least one breakpoint exists. This extends `MachineController.cpp:506-526` so a TRACE file capture and the debugger coexist. History needs no flag; the ring is always on.
    - **Pause indicator:** the window title and a DebugPanel status line show "Paused (debugger)".
    - **Rebuild handling:** `discardMachine()` and rebuild notify the session.
11. **Settings** (`AppSettings.hpp`):
    - Keys: `debug/dapEnabled` (default false) and `debug/dapPort` (default 4711).
    - `SettingsDialog.cpp` gets a new "Debugger" section with a QCheckBox, a QSpinBox (1024–65535) and a live status label: "Listening on 127.0.0.1:4711", "Client connected" or "Port in use". Its objectName is set so screenshot scenarios can crop it.
    - Changes go through `MachineController::refreshDebugServer()`, which starts, stops or rebinds the server. This follows the `refreshSerialLinkDirectory()` pattern.
12. **Attach arguments** (all optional; with none, the session is plain ROM research on the running machine):
    - `listings: [{path, cpu?, bank?, me?, pu?, pv?}]`: static listings, e.g. for ROM code.
    - `symbols: [path]`
    - `program`: a load descriptor, see Build & Load.
    - `preset: path`: runs a preset through `PresetRunner` first.
    - `reset: "none" | "reset" | "allReset"` (default `none`) and `stopOnEntry`.

### Build & Load
- **One load descriptor**, used by the attach argument `program` and by the custom request `calcu1600/load`:
  ```json
  { "bin": "…/prog.bin", "listing": "…/prog.rst", "symbols": ["…/prog.sym"],
    "cpu": "z80|lh5801|lh5803", "address": "0x40C5", "slot": "S0",
    "bank": 3, "me": 0, "pu": 0, "pv": 1,
    "entry": "0x40C5", "after": "none|call|stopOnEntry" }
  ```
- **Loading** reuses `machinecode::readFile`/`plan`/`pc1600TargetFor` (`Core/MachineCodeFile.hpp`), `PC1500MachineCodeLoader` and `PC1600MachineCodeLoader`: the "Load Machine Code…" paths without the dialog, since the address comes from the descriptor or the header.
- **Binding:** a successful load binds the listing to the loaded range plus its `BankKey` and runs byte verification (Core item 7).
- **`after`:** `call` types the `CALL` to the entry address; `stopOnEntry` sets a temporary breakpoint at the entry first.
- **Rebuild during a session** (`calcu1600/load`): pause, load the new binary, rebind the listing, re-resolve the stored source breakpoints of that file (the addresses moved) and send `breakpoint` `changed` events, then do what `after` says.

### ROM research (no program of our own)
- **Attach without `program`.** The session relies on static `listings`/`symbols` and on the disassembly view; pause, breakpoints, stepping, memory and data breakpoints work the same.
- **Reset and stop at the very first instruction:** `reset` + `stopOnEntry` at attach, or `calcu1600/reset` / DAP `restart` during a session. The DebugTarget's one-shot "stop before next instruction" flag is armed before `reset()`/`allReset()`, and the stop has reason `entry`.
  - PC-1500: at the LH5801 reset-vector target.
  - PC-1600: at Z80 0000H. The LH5803 thread shows its own state and can be stepped on its own.
- The history ring is cleared on reset.
- Static ROM listings carry `BankKey` qualifiers (`pu`/`pv` for LH580x banked ROMs, `bank` for PC-1600 ROM banks) and get byte verification, so a listing for the wrong ROM version is flagged stale.

### VS Code extension (in repo: `vscode/calcu1600-debug/`)
- `package.json` and a plain `extension.js` with no build step. It registers debug type `calcu1600` with a `DebugAdapterDescriptorFactory` that returns `new vscode.DebugAdapterServer(port, "127.0.0.1")`.
- `contributes.breakpoints` enables breakpoints for the common asm language IDs (`lh5801-asm`, `asm`, `z80-asm`, `z80-macroasm`, `plaintext` fallback), with the `debug.allowBreakpointsEverywhere` hint in the README.
- Commands: "Calc-U-1600: Build & Load" (bound to a key: runs the launch config's `buildTask`, then sends `customRequest('calcu1600/load', program)`), "Reset & Stop" and "All Reset & Stop".
- Launch config fields use variables such as `${fileDirname}/${fileBasenameNoExtension}.rst`; `preLaunchTask` covers the first build at attach.
- It provides an attach configuration schema, snippets for PC-1500 sdas and PC-1600 zasm, the task templates from the Toolchain section, and install steps: `npx @vscode/vsce package`, or a symlink into `~/.vscode/extensions` for development.

### Build and docs
- **New Core `.cpp` files** go into every source list: root `CMakeLists.txt` (`CORE_COMMON_SOURCES`, `CoreTests`), `Qt6/CMakeLists.txt` (`CORE_SOURCES`), and `tools/build_*.sh` / `tools/run_tests.sh`.
- **New app files** go into `qt_add_executable`.
- **`docs/Debugger.md`**, written in the same structure as `docs/PC1600-Serial-Port.md`: How it works, Using VS Code, Listings, Known limitations. It also gets a short User-Guide mention.
- **TODO.md:** retire the "BreakpointSet has one user" cleanup entry.

---

## Toolchain
Settled before implementation. Both assemblers and the parsers come from here. The listing parsers support exactly these tools and nothing else.

| CPU | Assembler | Listing parsers | Disassembler (debug API) |
|---|---|---|---|
| Z80 (SC7852) | **zasm** (primary), sdasz80 | `ZasmListing`, `SdasListing` | own `Z80Disassembler` |
| LH5801 / LH5803 | **sdaslh5801** | `SdasListing` | own `LH5801Disassembler` |

**Assemblers**
- **zasm**: a local checkout of `github.com/Megatokio/zasm`, run as `~/Development/sharp/zasm/zasm`. It's maintained (4.5.0), and the PC-1600 rom-dumper already uses it (`zasm -uwy src.asm src.lst src.bin`). The listing shows the object code next to the source, and the `-y` symbol table gives each label's `file:line`. That anchors include files for `ZasmListing`.
- **sdasz80 / sdaslh5801**: a local checkout of `github.com/pchambre/sdcc-pc1500`, run from `~/Development/sharp/sdcc-pc1500/sdcc/bin/`. Both CPUs share one `.lst`/`.rst`/`.sym` format and therefore one parser. `sdld` writes the relocated `.rst`. Use the checkout's own `sdasz80`, not Homebrew's, so both CPUs build with the same SDAS version. The build recipe (`sdas -plosgff` → `sdld` → `makebin -p`) is the one in `examples/memtest.asm`.
- **Not supported:**
  - tasm is retired.
  - lhasm (lhTools) has its own dialect and listing format.
  - z80asm (Bas Wijnen's 1.8 from 2007) is used by no source here.
  
  Each one would need its own parser and gain nothing.

**Disassemblers: our own, not an external tool.**
- The debug API needs structured decodes: the length, the flow kind and the target (for step over/out), plus symbol names.
- It decodes through side-effect-free peeks in the *currently mapped* bank.
- It must resync backwards for a `disassemble` request with a negative offset.
- A file-to-file CLI such as z80dasm or lhunasm can do none of this, and both are GPL, so they can't be linked in.
- The opcode tables come from the Zilog Z80 manual and the Sharp LH5801 TRM (see the source rule above).
- z80dasm remains an optional offline tool for static listings of whole ROM banks. It isn't a build or test dependency.

**Tool locations:** nothing is hardcoded in the repo. Tasks and launch configurations get the paths from two variables, with the checkouts above as defaults:
- `CALCU_ZASM`: the zasm binary.
- `CALCU_SDCC_BIN`: the sdcc-pc1500 `bin` directory.

**Editor integration**
- **VS Code:** ship `tasks.json` snippets with the extension (`vscode/calcu1600-debug/`). There is one build task per assembler, each with a problem matcher taken from real output:
  - **sdas:** a single-line matcher for `file.asm:LINE: Error: <x> message`.
  - **zasm:** a multi-line matcher. Errors come out as `LINE: <source>` followed by a caret line, `   ^ message`, and they don't name the file. The matcher takes the file from the task's `${file}`.
  - A `calcu1600` attach configuration uses the matching task as its `preLaunchTask` and passes the resulting `.lst`/`.rst` (and `.sym`) as `listings`/`symbols`.
- **CLion** (later): check whether current CLion attaches to a TCP DAP server natively; otherwise via a plugin. The same commands as External Tools or Shell Script run configurations, used as a "Before launch" step of the attach configuration.

---

## Phases
Each phase is committed to `dev-0.6.0` and I continue to the next without stopping.
1. **Always-on history ring with instruction bytes, and both disassemblers** (the LH5801 table also covers the LH5803), with tests: a full opcode-table sweep and known ROM sequences.
2. **CPU and machine debug hooks:**
   - SC7852 breakpoints, skip-once, and `PC1600Machine::runCycles` stops.
   - The register accessors and peeks.
   - Memory-only WatchSet with ME selector, and the fetch flag.
   - The DebugTarget implementations (including `bank`/`me`/`pu`/`pv` state and the stop-before-next flag) and DebugExpression.
   - Tests for each.
3. **Listing, symbol and SourceMap parsers** (sdas, zasm) behind `ListingSource`, `BankKey`, load and static-listing bindings with byte verification, with committed fixtures and tests.
4. **Minimal usable debugger:**
   - Qt6::Network, DapServer/DapSession/DebugController and the Settings section.
   - Pause integration.
   - threads, stackTrace, scopes and variables; source and instruction breakpoints with conditions; continue, pause and stepInstruction.
5. **Remaining requests:**
   - Step over/out and line stepping.
   - disassemble, read/write memory, setVariable and evaluate.
   - Data breakpoints, logpoints and function breakpoints.
   - Attach `program` load, `calcu1600/load` with breakpoint re-resolution.
   - `reset` + `stopOnEntry`, `calcu1600/reset` and DAP `restart`.
   - Machine-rebuild handling.
6. **VS Code extension** (Build & Load and reset commands, task templates), **docs and the DAP smoke script.**

## Verification
- **CoreTests** (`cmake --build build --target CoreTests && ctest --test-dir build --output-on-failure`):
  - Disassembler tables.
  - Listing parsers on the fixtures.
  - The expression evaluator.
  - SC7852 and LH5803 breakpoints stopping `PC1600Machine::runCycles`, and skip-once.
  - Watchpoint hits that exclude fetches.
  - History frames holding the correct bytes and post-execution registers with no TRACE flags set; cleared on reset.
  - `BankKey` evaluation for PC-1600 banks 0-7 and LH580x ME/PU/PV.
  - Binding replacement on overlapping loads; stale detection for loaded and static listings.
- **Regression checks:** the existing suite stays green, the PC-1500 CE-150 demo still gives 1254 pts, the PC-1600 timing tests are unchanged with the always-on ring, and TRACE.bin can still be read by `tools/read_trace.py`.
- **`tools/dap_smoke.py`**, run with `uv run`, stdlib only. It connects to the running app and runs initialize → attach (loading the `examples/memtest.pc1500` preset with its listing) → setBreakpoints on an `.asm` line → configurationDone. It then asserts the `stopped` event, a stackTrace with 21 frames and the mapped source line, the registers, a readMemory call, a step and continue. It then sends `calcu1600/load` with a rebuilt `.bin` whose code has shifted and asserts the breakpoint moved and still hits.
- **`dap_smoke.py`, ROM run:** attach with no program, `reset: "reset"` and `stopOnEntry`; assert the stop at the reset-vector target (PC-1500) or 0000H (PC-1600), step 5 instructions, and assert the history and the disassembly view show them.
- **Manual check in VS Code:**
  - PC-1500 `memtest.asm`, built via the pc1500-build recipe: breakpoint in `.asm`, conditional breakpoint (`A == 0x10`), step over a `SJP`, step out, edit a register, a data breakpoint on a RAM byte, and the memory hex view.
  - PC-1600 rom-dumper (zasm): a breakpoint in Z80 code, then continue into an LH5803 handoff. Confirm both threads, and that the stop reports the LH5803 thread.
  - Single-step the PC-1500 reset routine in the disassembly view.
  - Toggle the port and enable switch in Settings while a client is connected.
