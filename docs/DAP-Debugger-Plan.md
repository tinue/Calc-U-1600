# DAP debug server for Calc-U-1600

## Context
Machine-code work on the PC-1500 (LH5801) and PC-1600 (Z80 SC7852 + LH5803) is debugged today only by the TRACE file and CLI poking. We want the emulator to offer a **Debug Adapter Protocol** server over TCP. VS Code attaches to it (CLion later) and gets breakpoints (including conditional ones), stepping, registers, memory and disassembly. When a listing from an earlier assembly run exists, VS Code also shows the original `.asm` source.

The CPUs are presented as DAP **threads**: one on the PC-1500, two on the PC-1600. The trace ring buffer is presented as the **call stack**. Frame 0 is the live state at the next PC. Frames 1..20 are the last 20 executed instructions, each with its post-execution registers.

**Decisions already made with you:**
- **Source view:** show the original `.asm`, falling back to the disassembly view.
- **Listing formats:** sdaslh5801 `.lst`/`.rst`, sdasz80 `.lst`/`.rst`, zasm `.lst`, and `.SYMBOLS:`/`.sym` tables.
- **Session model:** attach, with an optional program or preset load first.
- **v1 features also include:** step over and step out, memory view and edit, register edit, and data breakpoints.
- **Ring semantics:** post-execution registers. This already matches the code: `recordTraceFrame` runs after `execute()` and records the pre-execution PC.

**Current state found by exploration:**
- **No disassembler** exists in the repo.
- **No emulation thread:** the emulator runs on the GUI thread from the `EmulationPacer` 16 ms QTimer, so a QTcpServer on the GUI thread needs no extra locking.
- **Breakpoints only on the LH5801:** `BreakpointSet` in `Core/CPU/TraceRing.hpp` is not used by the GUI, has no skip-once, and `PC1600Machine::runCycles` ignores it.
- **The SC7852 has no breakpoints**, no alternate-register accessors and no `peekTraceEvents`.
- **Missing peeks:** there is no ME1 or LH5803-side peek.
- **No Qt6::Network and no JSON library.** QJsonDocument arrives with Qt6::Core.
- **Trace frames lack operand bytes:** they hold only the opcode or prefix word, not the instruction bytes.

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
                              ├─ Listing parsers → SourceMap
                              ├─ DebugExpression (conditions, evaluate)
                              └─ DebugTarget (PC1500 / PC1600 impl: threads, regs, peek/poke, history, step)
```

### Core (Qt-free, unit-tested)
1. **Instruction bytes in trace frames.**
   - `CpuFrame` and `Z80CpuFrame` gain `uint8_t bytes[4]; uint8_t len`.
   - The CPUs record the bytes they fetch in the existing `fetch8`/`fetch16`/`fetchOpcode` helpers, using a small per-step buffer. This is cheap and correct even for self-modifying or banked code.
   - SC7852 split DD/FD prefix steps carry the prefix into the next frame's bytes.
   - `PC1500TraceFile` serialises fields explicitly, so the TRACE.bin format is unchanged.
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
   - **SC7852 history:** `peekTraceEvents`.
   - **Stop on breakpoint in `PC1600Machine::runCycles`:** today an LH5803 breakpoint would spin as a halt tick. `runCycles` must break on `consumeBreakpointHit()` from either CPU and report which CPU hit.
   - **Side-effect-free peeks:** `PC1500Memory` ME1 and `LH5803SharedMemory` (ME0/ME1) get peeks. The I/O windows read their register latch without clear-on-read effects.
4. **Data breakpoints.**
   - A `WatchSet` (address ranges plus read/write kind, with an atomic "any watches" fast path) is checked in each machine's bus wrappers: PC-1500 ME0/ME1, the PC-1600 Z80 memory bus, and the LH5803 bus.
   - Read watches ignore opcode and operand fetches: the CPU sets an `m_fetching` flag around its fetch helpers.
   - A hit latches the address and value. The machine stops after the current instruction, consistent with post-execution semantics.
5. **`Core/Debug/DebugTarget.hpp`**, with `PC1500DebugTarget` and `PC1600DebugTarget`.
   - Threads (CPU id and name) and which CPU owns the bus.
   - A register list with get/set by name, including the flag bits.
   - Peek/poke in a CPU's own address space, with an ME0/ME1 selector for the LH580x.
   - History of up to N frames per CPU, from the ring's `peek`, which is independent of the TRACE file drain cursor.
   - Bank context for listing qualifiers: the PC-1600 slot and page from `debugBankState()`, and PV/PU on the LH580x.
   - `stepInstruction(cpu)`: steps the machine until that CPU has retired one instruction, with a cap. On the PC-1600, stepping the CPU that does not own the bus runs until it does.
   - `runUntil(budget, stopPredicate)`: a per-instruction loop for step over/out and conditional stops.
6. **`Core/Debug/DebugExpression.{hpp,cpp}`**: a small C-like expression parser and evaluator.
   - It knows register names and flags per CPU, memory reads (`[addr]` for a byte, `w[addr]` for a word in CPU endianness), symbols, and hex (`0x`/`$`/`&`) and decimal literals.
   - Operators: `== != < <= > >= && || ! + - & | ^ << >>`.
   - It is used for breakpoint conditions, hit conditions, logpoint `{expr}` interpolation, watch/hover/REPL `evaluate`, and `setVariable` values.
7. **`Core/Debug/Listing/`** parsers produce one `SourceMap`.
   - `SdasListing` reads `.lst`/`.rst` for both sdaslh5801 and sdasz80: address, bytes, the source line-number column and the file. `.rst` is preferred when present because it holds the relocated addresses.
   - `ZasmListing` reads the `.lst` code lines and maps them 1:1 to source lines. The symbol table's `file:line` entries are used to anchor and verify that mapping, and to find include files.
   - `SymbolFile` reads `.SYMBOLS:` tables (`HHHH name` pairs) and sdas `.sym`.
   - `SourceMap` provides:
     - Lookups keyed by `(cpu, addr)`: address → (file, line), and (file, line) → addresses, snapping to the next line that has code.
     - A label ↔ address map.
     - An optional **bank qualifier** per listing (PC-1600 slot S0/S1/S2 or a page bank; LH580x PV/PU). A qualified breakpoint only hits while that bank is mapped, implemented as an implicit condition.
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

   | Request | Behaviour |
   |---|---|
   | `initialize` / `attach` / `configurationDone` / `disconnect` | Disconnect clears breakpoints and resumes. |
   | `threads` | PC-1500: `LH5801`. PC-1600: `Z80 (SC7852)` and `LH5803`. The name marks which one owns the bus. |
   | `stackTrace` | Frame 0 is the live PC. Frames 1..20 come from the ring, newest first, named `after C0EE  CALL KEYGET`. Each frame's `source`/`line` comes from the SourceMap when mapped, and `instructionPointerReference` is set on every frame. |
   | `scopes` / `variables` | **Registers**: live for frame 0, post-execution for history frames. It includes a flags child that expands to bits. The Z80 shows the main and alternate sets, I/R/IFF/IM. **Banks** (PC-1600 bank state, PU/PV) is read-only. |
   | `setVariable` | Frame 0 only; the value is parsed by DebugExpression. |
   | `setBreakpoints` / `setFunctionBreakpoints` / `setInstructionBreakpoints` | Source line → address via the SourceMap, snapping to the next code line and returning the `verified` line. `condition`, `hitCondition` and `logMessage` are supported. |
   | `setDataBreakpoints` / `dataBreakpointInfo` | Registers and memory addresses support `read`, `write` and `readWrite`. The data ID is `cpu:space:addr`. |
   | `continue` / `pause` / `next` / `stepIn` / `stepOut` / `stepBack` | `stepBack` is not supported. `stepIn` and `next` step one instruction when granularity is `instruction` or no source is mapped, and otherwise step until the source line changes. `next` over a Call-kind instruction runs to the return address with SP ≥ the entry SP. `stepOut` runs until a Return-kind instruction retires with SP above the entry SP. |
   | `disassemble` | Uses the thread's CPU and the SourceMap for `location`/`line`, and symbol labels. |
   | `readMemory` / `writeMemory` | `memoryReference` is `cpu:hexaddr`, using peek/poke with no side effects. |
   | `evaluate` | Watch, hover and REPL, through DebugExpression. |

   Events:
   - `stopped`, with reason `breakpoint`, `data breakpoint`, `step`, `pause` or `entry`, the threadId of the hitting CPU, `allThreadsStopped: true` and hit breakpoint IDs.
   - `continued` and `output` (for logpoints).
   - `thread` exited/started and `invalidated` on a machine rebuild. The adapter re-applies breakpoints to the new machine.
10. **`Qt6/app/debug/DebugController`**, owned by MachineController.
    - **Paused flag:** `EmulationPacer::onTick` skips `advance()` while paused but still refreshes views.
    - **Run modes:**
      - Free run uses `runCycles` with breakpoints. On a raw hit it evaluates condition, hit count and bank qualifier. If the result is false, it calls skip-once and continues within the same tick's budget.
      - Step, step over, step out and conditional stops use `DebugTarget::runUntil` with a predicate, time-sliced per tick so the GUI stays live.
    - **Trace flags:** while attached, the controller ORs `TRACE_FULL | TRACE_BREAKPOINTS` into the flags that `MachineController` computes. This extends `MachineController.cpp:506-526` so a TRACE file capture and the debugger coexist. Frames are captured whenever a client is connected.
    - **Pause indicator:** the window title and a DebugPanel status line show "Paused (debugger)".
    - **Rebuild handling:** `discardMachine()` and rebuild notify the session.
11. **Settings** (`AppSettings.hpp`):
    - Keys: `debug/dapEnabled` (default false) and `debug/dapPort` (default 4711).
    - `SettingsDialog.cpp` gets a new "Debugger" section with a QCheckBox, a QSpinBox (1024–65535) and a live status label: "Listening on 127.0.0.1:4711", "Client connected" or "Port in use". Its objectName is set so screenshot scenarios can crop it.
    - Changes go through `MachineController::refreshDebugServer()`, which starts, stops or rebinds the server. This follows the `refreshSerialLinkDirectory()` pattern.
12. **Attach + optional load.** The attach arguments are:
    - `listings: [{path, source?, cpu?, bank?}]`
    - `symbols: [path]`
    - `program: {path, format, address, slot}` (the same fields as the preset `program:` block)
    - `preset: path`
    - `stopOnEntry`

    Loading reuses `PresetRunner` and the machine-code loader paths behind `MachineCodeLoadDialog`. With `stopOnEntry`, a temporary breakpoint is set at the entry or auto-run address before the `CALL` is typed.

### VS Code extension (in repo: `vscode/calcu1600-debug/`)
- `package.json` and a plain `extension.js` with no build step. It registers debug type `calcu1600` with a `DebugAdapterDescriptorFactory` that returns `new vscode.DebugAdapterServer(port, "127.0.0.1")`.
- `contributes.breakpoints` enables breakpoints for the common asm language IDs (`lh5801-asm`, `asm`, `z80-asm`, `z80-macroasm`, `plaintext` fallback), with the `debug.allowBreakpointsEverywhere` hint in the README.
- It provides an attach configuration schema, snippets for PC-1500 sdas and PC-1600 zasm, and install steps: `npx @vscode/vsce package`, or a symlink into `~/.vscode/extensions` for development.

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
- **CLion** (later): the same commands as External Tools or Shell Script run configurations, used as a "Before launch" step of the attach configuration.

---

## Phases
Each phase is committed to `dev-0.6.0` and I continue to the next without stopping.
1. **Trace-frame instruction bytes and both disassemblers**, with tests: a full opcode-table sweep and known ROM sequences.
2. **CPU and machine debug hooks:**
   - SC7852 breakpoints, skip-once, and `PC1600Machine::runCycles` stops.
   - The register accessors and peeks.
   - WatchSet and the fetch flag.
   - The DebugTarget implementations and DebugExpression.
   - Tests for each.
3. **Listing, symbol and SourceMap parsers**, with committed fixtures and tests.
4. **Minimal usable debugger:**
   - Qt6::Network, DapServer/DapSession/DebugController and the Settings section.
   - Pause integration.
   - threads, stackTrace, scopes and variables; source and instruction breakpoints with conditions; continue, pause and stepInstruction.
5. **Remaining requests:**
   - Step over/out and line stepping.
   - disassemble, read/write memory, setVariable and evaluate.
   - Data breakpoints, logpoints and function breakpoints.
   - Attach + load and stopOnEntry.
   - Machine-rebuild handling.
6. **VS Code extension, docs and the DAP smoke script.**

## Verification
- **CoreTests** (`cmake --build build --target CoreTests && ctest --test-dir build --output-on-failure`):
  - Disassembler tables.
  - Listing parsers on the fixtures.
  - The expression evaluator.
  - SC7852 and LH5803 breakpoints stopping `PC1600Machine::runCycles`, and skip-once.
  - Watchpoint hits that exclude fetches.
  - History frames holding the correct bytes and post-execution registers.
- **Regression checks:** the existing suite stays green, the PC-1500 CE-150 demo still gives 1254 pts, and TRACE.bin can still be read by `tools/read_trace.py`.
- **`tools/dap_smoke.py`**, run with `uv run`, stdlib only. It connects to the running app and runs initialize → attach (loading the `examples/memtest.pc1500` preset with its listing) → setBreakpoints on an `.asm` line → configurationDone. It then asserts the `stopped` event, a stackTrace with 21 frames and the mapped source line, the registers, a readMemory call, a step and continue.
- **Manual check in VS Code:**
  - PC-1500 `memtest.asm`, built via the pc1500-build recipe: breakpoint in `.asm`, conditional breakpoint (`A == 0x10`), step over a `SJP`, step out, edit a register, a data breakpoint on a RAM byte, and the memory hex view.
  - PC-1600 rom-dumper (zasm): a breakpoint in Z80 code, then continue into an LH5803 handoff. Confirm both threads, and that the stop reports the LH5803 thread.
  - Toggle the port and enable switch in Settings while a client is connected.
