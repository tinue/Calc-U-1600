#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "PC1500Variant.hpp"

// ── Preset file model + parser (the "common loader": file open, parse, ──
//     model resolution -- see the 3-part structure note below) ───────────
//
// A hand-rolled parser for this project's `.pc1500` YAML preset format.
// Deliberately scoped: no rom-modules/check support (see
// PresetFile::memoryExpansionModule for what memory-expansion support
// does exist).
//
// PRESET LOADING IS THREE PARTS:
//   1. this file -- parsePresetFile() opens the file, parses it, and
//      resolves `model:` into `PresetFile` (`isPC1600` / `variant`). Model-
//      family-agnostic. The Bridge's +[PC1500MachineWrapper
//      presetInfoAtPath:] is a thin wrapper over it for the "peek the
//      model before building a machine" step.
//   2. the correct family loader takes the parsed PresetFile + a machine:
//      Core/PC1500/PC1500PresetLoader.cpp `applyPC1500Preset()` for
//      PC-1500/1500A, Core/PC1600/PC1600PresetLoader.cpp
//      `applyPC1600Preset()` for PC-1600.
//   3. the dispatch (peek model -> build the right wrapper -> call its
//      family loader) lives in EmulatorViewModel.loadPreset, the one place
//      both machine wrappers coexist.
//
// `memory-expansion:` accepts exactly one module -- `- module: ce155`,
// `- module: ce1638plus`, or `- module: ce163f` -- as throwaway proofs of
// concept for the Phase 4 connector layer (see
// PresetFile::memoryExpansionModule and applyPC1500Preset()); a PC-1600
// preset instead uses `memory-expansion-1:` / `memory-expansion-2:` (see
// slot1Module/slot2Module). Every other module name, extra field, or
// `rom-modules:`/`check` step remains rejected. `PC-1500`, `PC-1500A` and
// `PC-1600` models are accepted (see PresetFile::isPC1600 / variant). Not general
// YAML: flat `key: value` top-level mappings, `- key: value` sequence
// items (one verb per step, no further nesting), and one `text: |` block
// scalar. Step verbs: `key:`, `type:`, `wait:`, `trace:`, `screenshot:`, and
// `syncclock:`. `- wait: N`
// runs N seconds of emulated time; `- wait:` with no value blocks until the
// ROM's keyboard idle loop re-engages -- i.e. until a long-running program
// or plot has finished (a generous safety cap still applies). `trace:` is
// a port of Calc-U-59's `KEYSTROKES:` `Trace:` directive: `- trace:
// name.bin` starts a CPU instruction trace to `name.bin` under the
// configured trace directory (silently overwriting an existing file;
// starting a new one first closes the open one), `- trace: off` stops it,
// and any trace still open when the preset finishes is closed
// automatically. The filename must not contain a path separator. See
// PC1500PresetLoader.cpp's applyPC1500Preset(). `- screenshot: name.png`
// writes a PNG of the LCD dot matrix at that point in the script into the
// same trace directory (overwriting; same no-path-separator rule) -- the
// image Edit > Copy Screen puts on the clipboard, see
// Core/Display/LcdScreenshot.hpp. `- syncclock:` (no value) re-seeds the
// machine's real-time clock from the host's current local time at that
// point -- a preset load runs flat out, so put it last to undo the clock
// running ahead during the load. A value may be `"double"` or `'single'`-quoted -- stripped and
// passed through verbatim (see PresetFile.cpp's unquote()), kept for
// preset files with quoted values that predate this fork. Tabs, flow
// style, multiple documents, and inline `#` comments are all
// rejected/unsupported -- deliberately scoped to exactly the shape real
// preset files (this project's own samples) actually use, not a
// general-purpose YAML implementation.
//
// This project's loader treats a preset as an ordered SEQUENCE of
// `keys:`/`program:` blocks, executed top-to-bottom in file order -- so a
// preset can install a loader/firmware program, run it, then load and run
// a second payload program, all from one file (see
// examples/memtest_bank.pc1500a). `pre-load-keys:`/`post-load-keys:` are
// not recognized at all -- both become a single, repeatable `keys:` block
// name.
struct PresetStep {
    enum class Kind { Key, Type, Wait, Trace, Screenshot, SyncClock };
    Kind kind = Kind::Key;
    // key name (Key), program text (Type), or -- for Trace -- the trace
    // output filename to start capturing to, or "" to stop the current
    // capture (`- trace: off`). See PC1500PresetLoader.cpp's `trace:`
    // handling; a port of Calc-U-59's `KEYSTROKES:` `Trace:` directive.
    // For Screenshot, the PNG filename.
    std::string text;
    // (Wait) seconds of emulated time to run. A negative value is the
    // sentinel for a parameterless `- wait:` step: "run until the ROM's
    // keyboard-scan idle loop re-engages", i.e. block until a long-running
    // program or plot has finished, without the preset author guessing a
    // duration. The loader applies a generous safety cap.
    double waitSeconds = 0.0;
    static constexpr double kWaitUntilIdle = -1.0;
};

struct PresetProgram {
    // Binary      -- `format: binary`: raw machine-code bytes poked verbatim,
    //                no BASIC-pointer fix-up (a CALL payload). PC-1500 pokes
    //                the whole file at `address`. A PC-1600 preset instead
    //                gives `slot: S0|S1|S2` and loads linearly into that one
    //                slot; `address` / `length` default to a 16-byte PC-1600
    //                machine-language header (magic FF 10 00 00, type 0x10 --
    //                see Core/PC1600/PC1600MachineImage.hpp) when the file
    //                carries one, and each overrides its header field when
    //                given. A non-zero auto-run address in that header makes
    //                the loader type `CALL &<addr>` afterwards, so the
    //                machine must be in RUN mode at the end of the block
    //                (it is by default after boot; a preceding `keys:` block
    //                that went to PRO must `- key: mode` back first).
    // BasicText   -- `format: basic-text`: BASIC source typed in through the
    //                ROM's line editor (slow, but exact); `text` holds it.
    // BasicBinary -- `format: basic-binary` (alias `basic-tokenized`): a
    //                plain-text BASIC listing at `path`, tokenized in-process
    //                on load (via libsharpdx, headerless) into the run of
    //                in-RAM line records. The loader pokes it into the BASIC
    //                program area and fixes BASPRG_END -- fast, unlike the
    //                keystroke typer `basic-text` uses. `address` is unused
    //                (the base comes from the ROM's own pointers). Like
    //                `basic-text`, the preset must first leave the machine
    //                loadable -- a
    //                `keys:` section with `- key: cl` / `- type: NEW0`
    //                (PC-1600 also needs `- key: mode` for PRO). See
    //                Core/Basic/BasicBinaryImage.hpp and the model loaders.
    enum class Format { Binary, BasicText, BasicBinary };
    Format format = Format::Binary;
    std::string path;   // resolved absolute/relative-to-cwd path (Binary / BasicBinary, or BasicText loaded from a file)
    uint16_t address = 0; // Binary only -- load address (overrides a PC-1600 ML header's field)
    std::string text;   // BasicText only -- the program source, one statement per line

    // PC-1600 `format: binary` only. `slot` is required for a PC-1600
    // machine-language block; `S0` = internal RAM ($C000-$FFFF), `S1`/`S2`
    // = the two 40-pin memory slots ($8000-$BFFF window). `length` (bytes)
    // overrides the header's length field, and is required together with
    // `address` when the file has no PC-1600 ML header. `hasAddress` /
    // `hasLength` record whether the field was present in the preset (0 is
    // a legal explicit value).
    enum class Slot { None, S0, S1, S2 };
    Slot slot = Slot::None;
    uint32_t length = 0;
    bool hasAddress = false;
    bool hasLength = false;
};

// One `keys:` or `program:` block, in the file order it appeared.
struct PresetSection {
    enum class Kind { Keys, Program };
    Kind kind = Kind::Keys;
    std::vector<PresetStep> keys;   // Kind::Keys
    PresetProgram program;          // Kind::Program
};

struct PresetFile {
    std::string model;
    /// True for `model: PC-1600` -- derived from `model` rather than stored
    /// alongside it, so the two can't disagree. The PC-1600 is a separate
    /// machine (Core/PC1600/) with a fixed ROM set, its own two memory slots
    /// (memory-expansion-1:/memory-expansion-2: below) and no BASIC/binary
    /// program loading -- so a PC-1600 preset carries no `firmware:`, no
    /// `program:` section, and no `memory-expansion:` (unsuffixed) block, and
    /// `variant`/`romVariant` below are left at their defaults and unused.
    /// Applied by Core/PC1600/PC1600PresetLoader.cpp, not applyPC1500Preset().
    bool isPC1600() const { return model == "PC-1600"; }
    // Parsed from `model:` -- PC1500Variant::PC1500A for "PC-1500A",
    // PC1500Variant::PC1500 for "PC-1500". Whoever calls applyPC1500Preset() is
    // responsible for running it against a PC1500Machine already
    // constructed with this variant (variant is fixed at construction --
    // see PC1500Memory.hpp).
    PC1500Variant variant = PC1500Variant::PC1500A;
    // Which ROM revision to run, always one of "A01"/"A03"/"A04" -- never
    // a file path or a bare-model-dependent choice. There are only three
    // real options across all three machines (per the project owner):
    // the PC-1600 has exactly one ROM (no choice at all), the PC-1500A
    // can only run A04, and the PC-1500 (non-A) is the only model that
    // actually chooses between A01/A03/A04. For "PC-1500A", this
    // resolves unconditionally to "A04" regardless of any `firmware:`
    // field (accepted syntactically but genuinely ignored, since its
    // value can't matter for that model). For "PC-1500", it's read from
    // `firmware:` when present -- either a bare revision (`firmware:
    // A03`, the preferred form) or, kept for backward compatibility with
    // existing preset files, a
    // `.../PC-1500_A0N.ROM`-shaped path (only the "A0N" is ever extracted
    // from it) -- defaulting to "A04" when absent or matching neither
    // shape, matching AppSettings.swift's own default. WHERE the actual
    // ROM file for this variant lives is
    // deliberately not this struct's concern -- that's environment-
    // specific (a CLI tool's repo-relative `roms/` convention vs. a GUI
    // app's bundled resource lookup) and is left to whoever calls
    // applyPC1500Preset() (PC1500PresetLoader.hpp) to resolve.
    std::string romVariant;
    // `keys:`/`program:` blocks, in file order -- see PresetSection above
    // and the fork note at the top of this file. Applied in this exact
    // order by PC1500PresetLoader.cpp's applyPC1500Preset().
    std::vector<PresetSection> sections;
    // `memory-expansion:` -- empty if the preset had no memory-expansion
    // block, otherwise the declared module's name: `"ce155"`,
    // `"ce1638plus"`, or `"ce163f"`, the only three this loader accepts
    // (throwaway proofs of concept for the Phase 4 connector layer, not the
    // general Phase 7 software-defined module -- see PC1500PresetLoader.cpp's
    // applyPC1500Preset()). `rom-modules:` remains unsupported/rejected.
    std::string memoryExpansionModule;

    // PC-1600 only (`memory-expansion-1:` / `memory-expansion-2:`) -- the
    // module plugged into each 40-pin memory-slot connector, one of the
    // names in Core/Connector/SlotModuleFactory.hpp's `kSlotModuleNames`
    // (`"ce155"`, `"ram16"`, `"ram32"`, `"ce1638plus"`, `"ce163f"`), or
    // empty for an empty slot. See Core/PC1600/PC1600PresetLoader.cpp.
    std::string slot1Module;
    std::string slot2Module;

    // The pen-plotter/printer on the 60-pin system bus. `""` (key absent)
    // = none. Normalized to lower case; `plotter: none`/`off` -> `""`.
    //
    //  * PC-1600 preset: `"ce1600p"` (attached by applyPC1600Preset before
    //    the cold boot so the ROM sees it -- the loader needs the CE-1600P
    //    ROM path passed in). `"ce150"` also valid once the PC-1600
    //    LH5803-side CE-150 support lands (Phase 2); until then
    //    applyPC1600Preset rejects it.
    //  * PC-1500 / PC-1500A preset: `"ce150"` (attached by
    //    applyPC1500Preset before reset(); the loader needs the CE-150 ROM
    //    path passed in). `"ce1600p"` is a parse error -- that is a PC-1600
    //    device.
    std::string plotter;

    // PC-1600 only: `floppy: <name>` names a saved CE-1600F disk by its
    // `disk-name` (a `*.floppy.yaml` in the bundled or the user's save
    // directory, bundled first -- see Connector/FloppyImageFile.hpp) to load into
    // the floppy at attach time, instead of the default empty drive. An
    // optional `,A` or `,B` suffix (`floppy: mydisk,B`) selects which side
    // is facing the head once loaded (CE1600FCard::setSide()'s own
    // comment) -- stripped into `floppySide` below, so `floppy` itself is
    // always just the bare disk name. `""` (key absent) = no disk in the
    // drive, matching the GUI's "–empty–" default. Only valid alongside
    // `plotter: ce1600p` (attaching the CE-1600P always also attaches the
    // CE-1600F, per their union attach/detach --
    // PC1600Machine::attachCE1600P()); `floppy:` without `plotter:
    // ce1600p` is a parse error. See Core/PC1600/PC1600PresetLoader.cpp.
    std::string floppy;
    // 0 = side A (default), 1 = side B -- parsed from `floppy:`'s `,A`/`,B`
    // suffix. Meaningless when `floppy` is empty (no disk).
    int floppySide = 0;

    // Two `- ...:` alternatives to `- module:` in the same one-item block,
    // both naming a docs/Memory-Card-Definition-Format.md definition for
    // the general-purpose software-defined module. Exactly one of
    // `<...>Module` / `<...>ModuleSpecFile` / `<...>ModuleSpecName` is set
    // per block; all empty for a block that used `- module:` or for no
    // memory-expansion block at all. The loader (PC1500PresetLoader /
    // PC1600PresetLoader) turns whichever is set into a card via
    // Core/Connector/SoftwareDefinedCard.hpp's makeSoftwareDefinedCard().
    //
    //  * `- modulespecfile: <path>` -- a definition FILE. Resolved here to
    //    an absolute / cwd-relative path (like `program.path`, relative to
    //    the preset's own directory).
    std::string memoryExpansionModuleSpecFile;
    std::string slot1ModuleSpecFile;
    std::string slot2ModuleSpecFile;
    //  * `- modulespec: <module-name>` -- a bundled/standard module,
    //    referenced by the `module-name:` of its definition. Stored
    //    verbatim (unresolved); the loader looks it up in a caller-
    //    supplied module directory via
    //    Core/Connector/MemoryCardCatalog.hpp's resolveModuleSpecByName().
    std::string memoryExpansionModuleSpecName;
    std::string slot1ModuleSpecName;
    std::string slot2ModuleSpecName;
};

/// Parses the `.pc1500` preset file at `path` into `out`. File paths
/// inside the preset (`program.path`) are resolved relative to `path`'s
/// own directory. Returns false and fills `error` (including a line
/// number when available) on any parse failure or unsupported field; a
/// preset is parsed all-or-nothing, so a bad preset fails before the
/// machine is even started.
bool parsePresetFile(const std::string& path, PresetFile* out, std::string* error);
