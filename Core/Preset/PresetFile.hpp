#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../PC1500/PC1500Variant.hpp"

// ── Preset file model + parser (the "common loader": file open, parse, ──
//     model resolution -- see the 3-part structure note below) ───────────
//
// A hand-rolled parser for this project's `.pc1500` YAML preset format.
// Deliberately scoped: no rom-modules/check support.
//
// PRESET LOADING IS THREE PARTS:
//   1. this file -- parsePresetFile() opens the file, parses it, and
//      resolves `model:` into `PresetFile` (`isPC1600` / `variant`). Model-
//      family-agnostic.
//   2. the family loader arms and boots a machine for the parsed
//      PresetFile -- Core/PC1500/PC1500PresetLoader.cpp
//      `applyPC1500Preset()` for PC-1500/1500A,
//      Core/PC1600/PC1600PresetLoader.cpp `applyPC1600Preset()` for
//      PC-1600 -- then hands the `keys:` / `program:` sections to the
//      shared runPresetSections() (Core/Preset/PresetRunner.hpp).
//   3. the dispatch (peek model -> build the right machine -> call its
//      family loader) lives in the GUI's PresetController::loadPreset and
//      in each CLI.
//
// `memory-expansion:` accepts exactly one module, named by definition
// (`- modulespec: <module-name>` / `- modulespecfile: <path>`, see
// PresetFile::memoryExpansionModuleSpecName); a PC-1600 preset instead uses
// `memory-expansion-1:` / `memory-expansion-2:`. Every other extra field, or
// `rom-modules:`/`check` step remains rejected. `PC-1500`, `PC-1500A` and
// `PC-1600` models are accepted (see PresetFile::isPC1600 / variant). Not general
// YAML: flat `key: value` top-level mappings, `- key: value` sequence
// items (one verb per step, no further nesting), and one `text: |` block
// scalar. Step verbs: `key:`, `type:`, `wait:`, `trace:`, `screenshot:`,
// `syncclock:`, and `saveas:`. `- wait: N`
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
// `- saveas: s1:<name>` / `- saveas: s2:<name>` / `- saveas: floppy:<name>`
// saves the live battery card in slot 1/2, or the live floppy disk, to the
// environment's configured save directory under `<name>` -- the scripted
// counterpart of the control bar's "Name & Save" icon. `s2:`/`floppy:` are
// PC-1600 only (rejected on a PC-1500/1500A preset); `s1:` works on either
// model. Unlike the GUI's Name & Save, it works even when the slot/floppy
// was already saved/loaded from a named instance, and it silently
// overwrites an existing file of the same name -- so a preset can
// `saveas:` a card or floppy more than once, under different names, as it
// evolves through the script. See PC1600PresetLoader.hpp's
// PresetSaveAsFn (below) -- WHERE
// the save goes is environment-specific (the GUI's configured instance
// directory), so Core only parses the step; a caller that doesn't supply
// the callback gets a logged no-op.
//
// This project's loader treats a preset as an ordered SEQUENCE of
// `keys:`/`program:` blocks, executed top-to-bottom in file order -- so a
// preset can install a loader/firmware program, run it, then load and run
// a second payload program, all from one file (see
// examples/memtest_bank.pc1500a). `pre-load-keys:`/`post-load-keys:` are
// not recognized at all -- both become a single, repeatable `keys:` block
// name.
struct PresetStep {
    enum class Kind { Key, Type, Wait, Trace, Screenshot, SyncClock, SaveAs };
    Kind kind = Kind::Key;
    // key name (Key), program text (Type), or -- for Trace -- the trace
    // output filename to start capturing to, or "" to stop the current
    // capture (`- trace: off`). See PC1500PresetLoader.cpp's `trace:`
    // handling; a port of Calc-U-59's `KEYSTROKES:` `Trace:` directive.
    // For Screenshot, the PNG filename. For SaveAs, the name to save
    // under (see saveAsTarget below).
    std::string text;
    // (Wait) seconds of emulated time to run. A negative value is the
    // sentinel for a parameterless `- wait:` step: "run until the ROM's
    // keyboard-scan idle loop re-engages", i.e. block until a long-running
    // program or plot has finished, without the preset author guessing a
    // duration. The loader applies a generous safety cap.
    double waitSeconds = 0.0;
    static constexpr double kWaitUntilIdle = -1.0;

    // (SaveAs) which slot/device `text` (the name) should be saved under --
    // parsed from `- saveas: s1:<name>` / `s2:<name>` / `floppy:<name>`.
    // S2/Floppy are PC-1600 only; see parsePresetFile()'s per-model
    // validation.
    enum class SaveAsTarget { S1, S2, Floppy };
    SaveAsTarget saveAsTarget = SaveAsTarget::S1;
};

/// Fired for a `- saveas: s1:<name>` / `s2:<name>` / `floppy:<name>` step
/// (see the top-of-file doc comment) -- WHERE the save goes, and how to
/// splice/format it, are environment-specific (Qt6/app's AppPaths/
/// MemoryModuleManager/FloppyDiskManager), so Core only calls out here,
/// mirroring onArmed/onBooted. `target` is which slot/device to save
/// (always S1 on a PC-1500/1500A -- the parser rejects the others); `name`
/// is the name to save it under. Returns true on success, or false with
/// `*error` filled in -- a failure stops the preset exactly like any other
/// step failure. Left unset (the default) makes a `saveas:` step a logged
/// no-op, for a caller (CLI, tests) with no configured save directory.
using PresetSaveAsFn =
    std::function<bool(PresetStep::SaveAsTarget target, const std::string& name, std::string* error)>;

/// Runs one `saveas:` step through `onSaveAs` (both preset loaders share
/// this). Returns false with `*error` set ("saveas: ...") on failure.
inline bool runPresetSaveAsStep(const PresetStep& step, const PresetSaveAsFn& onSaveAs,
                                const std::function<void(const std::string&)>& log, std::string* error) {
    if (!onSaveAs) {
        if (log) log("  saveas: skipped (no save handler configured)");
        return true;
    }
    std::string saveError;
    if (!onSaveAs(step.saveAsTarget, step.text, &saveError)) {
        if (error) *error = "saveas: " + saveError;
        if (log) log("  saveas: FAILED: " + saveError);
        return false;
    }
    if (log) log("  saveas: -> \"" + step.text + "\"");
    return true;
}

struct PresetProgram {
    // Binary      -- `format: binary`: machine-code bytes poked verbatim,
    //                no BASIC-pointer fix-up (a CALL payload). The file may
    //                carry the machine's own machine-code header -- CE-158
    //                on a PC-1500/1500A, the 16-byte PC-1600 one on a PC-1600
    //                (Core/MachineCodeFile.hpp) -- which supplies the load
    //                address and length; `address` / `length` each override
    //                their header field, and a headerless file needs
    //                `address` (its length defaults to the whole file). A
    //                PC-1600 preset also gives `slot: S0|S1|S2` and loads
    //                linearly into that one slot. A non-zero auto-run
    //                address in the header makes the loader type
    //                `CALL &<addr>` afterwards, so the machine must be in RUN
    //                mode at the end of the block (it is by default after
    //                boot; a preceding `keys:` block that went to PRO must
    //                `- key: mode` back first).
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
    uint16_t address = 0; // Binary only -- load address (overrides the file header's)
    std::string text;   // BasicText only -- the program source, one statement per line

    // `format: binary` only. `slot` is required for a PC-1600
    // machine-language block (and rejected on a PC-1500); `S0` = internal
    // RAM ($C000-$FFFF), `S1`/`S2` = the two 40-pin memory slots
    // ($8000-$BFFF window). `length` (bytes) overrides the header's length
    // field, or the whole-file length of a headerless file. `hasAddress` /
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
    /// machine (Core/PC1600/) with its own two memory slots
    /// (memory-expansion-1:/memory-expansion-2: below); its calculator ROM
    /// version rides on the model (`model: PC-1600:old`, default `new`;
    /// stored in `romVariant`) and it takes no `memory-expansion:`
    /// (unsuffixed) block. `model` itself always holds the bare name.
    /// `variant` below is unused.
    /// Applied by Core/PC1600/PC1600PresetLoader.cpp, not applyPC1500Preset().
    bool isPC1600() const { return model == "PC-1600"; }
    // Parsed from `model:` -- PC1500Variant::PC1500A for "PC-1500A",
    // PC1500Variant::PC1500 for "PC-1500". Whoever calls applyPC1500Preset() is
    // responsible for running it against a PC1500Machine already
    // constructed with this variant (variant is fixed at construction --
    // see PC1500Memory.hpp).
    PC1500Variant variant = PC1500Variant::PC1500A;
    // Parsed from the ROM suffix of `model: NAME[:ROM]`.
    // PC-1600: "new" or "old" (calculator ROM version, default "new"), from
    // `model: PC-1600:old`.
    // PC-1500/1500A: which ROM revision to run, always one of "A01"/"A03"/"A04"
    // -- never a file path. There are only three real options across all
    // three machines: the PC-1600 chooses "new"/"old" instead (see above),
    // the PC-1500A can only run A04 (`model: PC-1500A:A04`; any other suffix
    // is a parse error), and the PC-1500 (non-A) is the only model that
    // actually chooses between A01/A03/A04 (`model: PC-1500:A03`). With no
    // suffix it defaults to "A04". (The old `firmware:` key is gone -- it is
    // a parse error pointing at this syntax.) WHERE the actual ROM file for
    // this variant lives is deliberately not this struct's concern -- that's
    // environment-specific (a CLI tool's repo-relative `roms/` convention vs.
    // a GUI app's bundled resource lookup) and is left to whoever calls
    // applyPC1500Preset() (PC1500PresetLoader.hpp) to resolve.
    std::string romVariant;
    // `keys:`/`program:` blocks, in file order -- see PresetSection above
    // and the fork note at the top of this file. Applied in this exact
    // order by PC1500PresetLoader.cpp's applyPC1500Preset().
    std::vector<PresetSection> sections;
    // The pen-plotter/printer on the 60-pin system bus. `""` (key absent)
    // = none. Normalized to lower case; `plotter: none`/`off` -> `""`.
    //
    //  * PC-1600 preset: `"ce1600p"` (`plotter: ce1600p[:new|old]`, ROM in
    //    `ce1600pRomVariant`; attached by applyPC1600Preset before
    //    the cold boot so the ROM sees it -- the loader needs the CE-1600P
    //    ROM path passed in). `"ce150"` also valid once the PC-1600
    //    LH5803-side CE-150 support lands (Phase 2); until then
    //    applyPC1600Preset rejects it.
    //  * PC-1500 / PC-1500A preset: `"ce150"` (attached by
    //    applyPC1500Preset before reset(); the loader needs the CE-150 ROM
    //    path passed in). `"ce1600p"` is a parse error -- that is a PC-1600
    //    device.
    std::string plotter;
    // The serial / parallel interface on the 60-pin system bus. `""` (key
    // absent, or `interface: none`/`off`) = none; `"ce158"` = the CE-158
    // (`interface: ce158` / `ce-158`). It sits alongside `plotter: ce150`
    // or alone, on a PC-1500 / PC-1500A or a PC-1600 (LH5803 side, MODE 1;
    // not together with `plotter: ce1600p`).
    std::string interfaceName;
    // PC-1600 only: "new" or "old" -- the CE-1600P ROM version, from
    // `plotter: ce1600p:old` (default "new", also when there is no plotter).
    // Independent of `romVariant`; the CE-1600F in the same box follows it.
    // Any ROM suffix on another plotter is a parse error.
    std::string ce1600pRomVariant = "new";

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

    // The module in `memory-expansion:` (PC-1500/1500A) or in
    // `memory-expansion-1:` / `memory-expansion-2:` (the PC-1600's two
    // memory slots): a one-item block naming a
    // docs/Memory-Card-Definition-Format.md definition in one of two ways.
    // At most one of `<...>ModuleSpecFile` / `<...>ModuleSpecName` is set
    // per block; both empty for no block (an empty slot). The loader (PC1500PresetLoader /
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
