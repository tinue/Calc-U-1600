#pragma once
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../Basic/BasicLoadResults.hpp"
#include "../Connector/ExpansionCard.hpp"
#include "../Connector/MemoryCardDefinition.hpp"
#include "../Display/LcdText.hpp"
#include "../HostClock.hpp"
#include "../MachineCodeFile.hpp"
#include "PresetFile.hpp"

// ── The model-independent half of applying a preset ─────────────────────
//
// applyPC1500Preset() / applyPC1600Preset() each arm and boot their own
// machine (ROM, modules, peripherals, reset) and then hand the preset's
// `keys:` / `program:` sections to runPresetSections(), which walks them in
// file order. The few things that genuinely differ per machine -- key
// handling, the typer, the BASIC loaders, the screenshot, how a step's
// state is logged -- come in through a PresetMachine adapter each loader
// implements over its own machine type.

/// Optional per-step progress sink -- each call is one already-formatted,
/// human-readable line (no trailing newline). The GUI wires this to its
/// "preset" log category so a stuck load can be compared line by line
/// against what's on the calculator's LCD; the CLIs print it to stderr.
/// Left unset (the default) it costs nothing.
using PresetLogFn = std::function<void(const std::string&)>;

/// Optional callback fired exactly once, right after the machine has been
/// armed, reset and settled past its boot sequence -- i.e. once it's a fully
/// live, running machine -- but *before* any of the preset's own
/// `keys:`/`program:` steps start executing. A GUI can use this to swap the
/// visible/active machine over at this point rather than waiting for the
/// whole (possibly many-seconds-long) script to finish: the boot itself is
/// fast, so a model switch becomes visible immediately and the preset's own
/// keystrokes/program typing then play out live on the already-switched-to
/// machine. Left unset (the default) costs nothing and changes no behavior.
using PresetBootedFn = std::function<void()>;

struct PresetLoadResult {
    bool ok = false;
    std::string error;
    /// From a typed `program:` block: source lines the machine's line
    /// editor didn't store (see BasicTypeResult::rejectedLines). Non-empty
    /// implies `ok == false`.
    std::vector<std::string> rejectedBasicLines;
    /// The on-disk file a `slot-N:`/`slot-N-file:` reference resolved
    /// to, per slot -- empty for an empty slot. The PC-1500/1500A has one
    /// expansion slot and reports it as slot 1. (Which module it is, the GUI
    /// reads from the slot itself.) Lets the GUI tell a bundled read-only
    /// template apart from a real saved battery-card instance (a file under
    /// the writable instance directory) so it can decide whether the
    /// attached module should autosave.
    std::string slot1ResolvedPath;
    std::string slot2ResolvedPath;  // PC-1600 only
    /// True when the preset had `plotter: CE-150` and the CE-150 was attached
    /// before reset, so the boot ROM's peripheral scan saw it. (PC-1600
    /// `plotter: CE-1600P` is reported via `machine.ce1600pAttached()`.)
    bool ce150Attached = false;
    /// True when the preset had `interface: CE-158` and the CE-158 was
    /// attached before reset, like the CE-150.
    bool ce158Attached = false;
    /// PC-1600 only: the preset's `floppy:` name, verbatim -- empty if the key
    /// was absent (the drive stays empty). For the GUI to resync its
    /// disk-picker without re-attaching anything.
    std::string floppyImageLabel;
    /// The on-disk `*.floppy.yaml` file `floppyImageLabel` resolved to, if
    /// any -- same purpose as slot1ResolvedPath.
    std::string floppyResolvedPath;
};

/// Optional callback fired exactly once, right after the machine has its ROM
/// loaded and its modules/peripherals attached, but *before* reset -- i.e.
/// fully "armed" yet still powered off. Also fired when arming fails part
/// way (then `armedSoFar.error` is set and the load returns right after).
/// `armedSoFar` is the in-progress result; its resolved paths and
/// `ce150Attached`/`ce158Attached` are already final, so a GUI can resync its slot selector and plotter-paper visibility
/// and repaint the armed-but-off machine before the (possibly long) boot and
/// preset script run. Left unset (the default) costs nothing.
using PresetArmedFn = std::function<void(const PresetLoadResult& armedSoFar)>;

/// What runPresetSections() needs from one machine. Each preset loader
/// implements it over its own machine type (via PresetMachineBase below).
class PresetMachine {
public:
    virtual ~PresetMachine() = default;

    /// Emulated cycles per second -- the unit of runCycles().
    virtual uint32_t cyclesPerSecond() const = 0;
    virtual uint64_t runCycles(uint64_t cycles) = 0;
    /// Blocks until the BASIC interpreter is back in its command loop (a RUN,
    /// a plot, a SAVE ... has finished), or `maxCycles`. Returns cycles spent.
    virtual uint64_t waitUntilBasicIdle(uint64_t maxCycles) = 0;
    /// Appended to every step's log line: the ROM's input line, the LCD as
    /// text and where the CPU is.
    virtual std::string stepTag() = 0;

    /// A `key:` step: `break`/`on` (the ON key) or one named key.
    virtual bool key(const std::string& name, std::string* error) = 0;
    /// A `type:` step: the line as keystrokes, then ENTER.
    virtual bool typeLine(const std::string& line, std::string* error) = 0;

    virtual bool cpuTraceActive() const = 0;
    virtual void beginCpuTrace(std::FILE* fh, uint32_t flags) = 0;
    virtual void endCpuTrace() = 0;
    /// A `screenshot:` step: PNG of the LCD at `path`.
    virtual bool writeScreenshot(const std::string& path, std::string* error) = 0;
    /// The LCD as text (`expect:` steps, the step log).
    virtual LcdText lcdText() = 0;
    /// A `syncclock:` step: re-seed the RTC from the host; returns the time set.
    virtual std::tm syncClock() = 0;

    /// A typed program (`text: |`, `typed: true`): type it in through the ROM's editor.
    virtual BasicTypeResult typeBasicProgram(const std::string& text) = 0;
    /// A BASIC `file:`: load the listing or tokenized file at `path`
    /// (loadBasicProgramFile(): on the PC-1600 the MODE picks the keyword
    /// table and the TITLE area is the target).
    virtual BasicLoadResult loadBasicFile(const std::string& path) = 0;
    /// A machine-code `file:`: which machine-code header family this machine
    /// takes (machinecode::headerMismatch()), the PC-1600's MODE / TITLE /
    /// program areas the placement follows (PC-1500: none), and the write
    /// of `len` bytes at the Z-80 / LH5801 bus address `busAddr` -- on the
    /// PC-1600 into the target `slot` and `bank` machinecode::planLoad() picked.
    virtual machinecode::Target codeTarget() const = 0;
    virtual machinecode::PC1600State codeState() { return {}; }
    virtual bool loadMachineCode(machinecode::Slot slot, int bank, uint32_t busAddr, const uint8_t* data, size_t len,
                                 std::string* error) = 0;
};

/// The ROM's typed-input line buffer (80 bytes from `base`, up to its CR)
/// as printable ASCII -- logged after each preset step so a load that goes
/// wrong can be lined up against the LCD's edit line.
template <class Machine>
std::string presetInputLine(Machine& machine, uint16_t base) {
    std::string s;
    for (uint16_t i = 0; i < 80; i++) {
        const uint8_t b = machine.memory().peek(static_cast<uint16_t>(base + i));
        if (b == 0x0D) break;
        s += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
    }
    return s;
}

/// BREAK (the ON key) isn't part of either key matrix, so it can't go
/// through tapKey(): press and release the ON line directly, holding each
/// state for `holdCycles`.
template <class Machine>
void presetTapOn(Machine& machine, uint64_t holdCycles) {
    machine.setOnKeyPressed(true);
    machine.runCycles(holdCycles);
    machine.setOnKeyPressed(false);
    machine.runCycles(holdCycles);
}

/// The parts of PresetMachine every machine forwards the same way.
template <class Machine>
class PresetMachineBase : public PresetMachine {
public:
    explicit PresetMachineBase(Machine& machine) : m_machine(machine) {}

    uint64_t runCycles(uint64_t cycles) override { return m_machine.runCycles(cycles); }
    bool cpuTraceActive() const override { return m_machine.cpuTraceActive(); }
    void beginCpuTrace(std::FILE* fh, uint32_t flags) override { m_machine.beginCpuTrace(fh, flags); }
    void endCpuTrace() override { m_machine.endCpuTrace(); }
    std::tm syncClock() override { return seedClockFromHostTime(m_machine); }

protected:
    Machine& m_machine;
};

/// The `slot-N: <name>` search path: `moduleDir` (the bundled
/// catalogue) first, then `extraModuleDirs` (the GUI's saved-cards folder,
/// the CLIs' repeated `--modules-dir`).
std::vector<std::string> presetModuleDirs(const std::string& moduleDir,
                                          const std::vector<std::string>& extraModuleDirs);

/// Builds the software-defined card a `slot-N-file:` path (`specFile`) or
/// `slot-N:` name (`specName`, looked up in `moduleDirs`) names, for
/// `host`. Returns null with `error` set on failure; `resolvedPath` receives
/// the spec file used.
std::unique_ptr<ExpansionCard> makePresetModuleCard(const std::string& specFile, const std::string& specName,
                                                    const std::vector<std::string>& moduleDirs, CardHost host,
                                                    std::string* resolvedPath, std::string* error);

/// Walks `preset.sections` in file order on the already-booted `machine`:
/// each `keys:` block's steps, each `program:` block's load. On success sets
/// `result->ok`; on the first failure returns with `result->error` set. A
/// `- trace:` capture still open when this returns is closed. `traceDir` is
/// where `trace:` and `screenshot:` steps write; `onSaveAs` runs `saveas:`
/// steps (see PresetSaveAsFn).
void runPresetSections(PresetMachine& machine, const PresetFile& preset, PresetLoadResult* result,
                       const PresetLogFn& log, const std::string& traceDir, const PresetSaveAsFn& onSaveAs);
