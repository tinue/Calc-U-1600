#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "../MachineCodeFile.hpp"
#include "../PC1500/PC1500Variant.hpp"
#include "../Yaml.hpp"

// ── Preset file model + parser (the "common loader": file open, parse, ──
//     model resolution -- see the 3-part structure note below) ───────────
//
// A hand-rolled parser for the preset format (`.pc1500`, `.pc1500a`,
// `.pc1600`; docs/User-Guide.md chapter 8 is the reference). YAML-shaped,
// not YAML (docs/background/Decisions.md): flat `key: value` top-level
// fields, `- verb: value` steps, one `text: |` block scalar, and a `type:`
// step that is typed exactly as written to the end of the line (`PRINT #1`
// keeps its `#`, quotes are typed). Only `debug:` and `bus-rom:` are YAML
// proper and go to Core/Yaml.hpp. Tabs, flow style and multiple documents
// are rejected.
//
// The naming rules:
//   * Sharp's product names, hyphen included, in any case: `model: PC-1600`,
//     `plotter: CE-1600P`, `interface: CE-158`. A ROM choice rides on the
//     name after a colon (`PC-1500:A03`, `PC-1600:old`, `CE-1600P:old`).
//   * A device by name, or by file: `slot-1: CE-1600M` / `slot-1-file:
//     my.card.yaml`, `floppy: Formatted` / `floppy-file: my.floppy.yaml`.
//     A path-valued key is `file` or ends in `-file`, except `host-drive:`,
//     a folder. Paths are relative to the preset; `~/` is the home folder.
//   * `saveas:` names its device with the same words: `slot-1`, `slot-2`,
//     `floppy`.
//   * Numbers: `&`, `0x` or `$` makes them hex, otherwise decimal.
//   * Leaving a key out means "none"; there is no `none` value.
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
// Step verbs: `key:`, `type:`, `wait:`, `trace:`, `screenshot:`,
// `syncclock:`, `saveas:`. `- wait: N` runs N seconds of emulated time;
// `- wait:` with no value blocks until the ROM's keyboard idle loop
// re-engages -- i.e. until a long-running program or plot has finished (a
// generous safety cap still applies). `- trace: name.bin` starts a CPU
// instruction trace to `name.bin` under the configured trace directory
// (overwriting; starting a new one first closes the open one), `- trace:
// off` stops it, and a trace still open when the preset finishes is closed.
// `- screenshot: name.png` writes the LCD (the image Edit > Copy Screen
// puts on the clipboard, Core/Display/LcdScreenshot.hpp) into the same
// directory. Neither file name may contain a path separator. `- syncclock:`
// re-seeds the real-time clock from the host's local time -- a preset load
// runs flat out, so put it last. A ` # comment` after a value is stripped,
// except after `type:`.
//
// `- saveas: template|live <device>:<name>` saves the battery card in
// `slot-1` / `slot-2`, or the floppy disk (`floppy`) -- the scripted
// counterpart of the control bar's "Name & Save". The leading word is
// required:
//   * `live`     -- an ordinary instance: the GUI autosaves later changes
//                   into it.
//   * `template` -- written with `template: true`: read-only from then on,
//                   every use starts from the saved contents.
// `<name>` saves into the environment's configured save directory under that
// module-name / disk-name. `file:<path>` instead writes exactly that file,
// relative to the preset's own directory; the path must end in `.card.yaml`
// (slots) or `.floppy.yaml` (floppy), and the name is the file name without
// that suffix:
//   - saveas: template slot-2:file:CE-1601M - Progs.card.yaml
//   - saveas: live floppy:Progs
// `slot-2`/`floppy` are PC-1600 only. Unlike the GUI's Name & Save, it works
// even when the slot/floppy was already saved/loaded from a named instance,
// and it silently overwrites an existing file of the same name (a template
// too). WHERE a by-name save goes is environment-specific, so Core only
// parses the step and hands a PresetSaveAsRequest to PresetSaveAsFn (below);
// a caller that doesn't supply the callback gets a logged no-op.
//
// A preset is an ordered SEQUENCE of `keys:`/`program:` blocks, executed
// top-to-bottom in file order -- so a preset can install a loader program,
// run it, then load and run a second payload (see
// examples/setup/firmware_bootstrap_util_15.pc1500a).
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
    // parsed from `- saveas: template|live slot-1:<name>` / `slot-2:` /
    // `floppy:`. Slot2/Floppy are PC-1600 only; see parsePresetFile()'s
    // per-model validation.
    enum class SaveAsTarget { Slot1, Slot2, Floppy };
    SaveAsTarget saveAsTarget = SaveAsTarget::Slot1;
    // (SaveAs) `template` (true) or `live` (false).
    bool saveAsTemplate = false;
    // (SaveAs) the `file:<path>` form: the file to write, resolved relative
    // to the preset's directory. Empty = save by name into the save folder.
    std::string saveAsPath;
};

/// One `saveas:` step, as handed to PresetSaveAsFn.
struct PresetSaveAsRequest {
    PresetStep::SaveAsTarget target = PresetStep::SaveAsTarget::Slot1;
    std::string name;      // module-name / disk-name to save under
    std::string path;      // `file:` form: the file to write; "" = by name, into the save folder
    bool isTemplate = false;
};

/// Fired for a `- saveas:` step (see the top-of-file doc comment) -- WHERE a
/// by-name save goes, and how to splice/format it, are environment-specific
/// (Qt6/app's AppPaths/MemoryModuleManager/FloppyDiskManager, or
/// PC1600PresetMedia.hpp for the headless tools), so Core only calls out
/// here, mirroring onArmed/onBooted. `request.target` is always S1 on a
/// PC-1500/1500A (the parser rejects the others). Returns true on success,
/// or false with `*error` filled in -- a failure stops the preset exactly
/// like any other step failure. Left unset (the default) makes a `saveas:`
/// step a logged no-op, for a caller (tests) with no configured save
/// directory.
using PresetSaveAsFn = std::function<bool(const PresetSaveAsRequest& request, std::string* error)>;

/// Runs one `saveas:` step through `onSaveAs` (both preset loaders share
/// this). Returns false with `*error` set ("saveas: ...") on failure.
inline bool runPresetSaveAsStep(const PresetStep& step, const PresetSaveAsFn& onSaveAs,
                                const std::function<void(const std::string&)>& log, std::string* error) {
    if (!onSaveAs) {
        if (log) log("  saveas: skipped (no save handler configured)");
        return true;
    }
    PresetSaveAsRequest request;
    request.target = step.saveAsTarget;
    request.name = step.text;
    request.path = step.saveAsPath;
    request.isTemplate = step.saveAsTemplate;
    std::string saveError;
    if (!onSaveAs(request, &saveError)) {
        if (error) *error = "saveas: " + saveError;
        if (log) log("  saveas: FAILED: " + saveError);
        return false;
    }
    if (log)
        log(std::string("  saveas: ") + (request.isTemplate ? "template" : "live") + " -> \"" + request.name + "\"" +
            (request.path.empty() ? "" : " (" + request.path + ")"));
    return true;
}

struct PresetProgram {
    // What the parser made of the block (parseProgramBlock()):
    // Binary      -- `file:` holding machine code: behind the machine's own
    //                header (CE-158 on a PC-1500/1500A, the 16-byte PC-1600
    //                one; Core/MachineCodeFile.hpp), which supplies the load
    //                address and length, or headerless, which needs
    //                `address:`. `address:` / `length:` each override their
    //                header field. On the PC-1600 the loader places code by
    //                MODE, TITLE and the address, as Load Machine Code does
    //                (docs/background/plans/Loader-Mode-Plan.md); `address:`
    //                is an LH5803 address in MODE 1. A non-zero auto-run
    //                address in the header makes the loader type the `CALL`
    //                afterwards, so the machine must be in RUN mode by then.
    // BasicBinary -- `file:` holding BASIC, a listing (tokenized in-process
    //                via libsharpdx) or tokenized behind a CE-158 / PC-1600
    //                header: poked into the program area as in-RAM line
    //                records, BASPRG_END fixed -- fast. The preset must
    //                first leave the machine loadable (`NEW0`, and PRO mode
    //                on the PC-1600). Core/Basic/BasicProgramSource.hpp.
    // BasicText   -- `text: |`, or `file:` + `typed: true` on a listing:
    //                typed in through the ROM's line editor (slow, exact);
    //                `text` holds the listing.
    enum class Format { Binary, BasicText, BasicBinary };
    Format format = Format::Binary;
    std::string path;     // `file:`, resolved (Binary / BasicBinary)
    uint16_t address = 0; // Binary only -- load address (overrides the file header's)
    std::string text;     // BasicText only -- the program source, one line per BASIC line

    // Binary only. `length` (bytes) overrides the header's length field, or
    // the whole-file length of a headerless file. `hasAddress` /
    // `hasLength` record whether the field was present (0 is a legal value).
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

/// One `bus-rom:` item: a ROM file on the 60-pin bus (Connector/BusRomCard.hpp).
/// Either `bank` (PC-1600 system bus, page B bank 4-7) or `address` (PC-1500
/// connector, or the PC-1600's LH5803 side) with its `me` / `pv` / `pu` gates.
struct PresetBusRom {
    std::string path;  // resolved against the preset's directory; read at load time
    int bank = -1;     // 4-7, or -1 = `address`
    uint16_t address = 0;
    bool me1 = false;  // `me: 1`
    int pv = -1, pu = -1;  // 0 / 1, or -1 = either
};

struct PresetFile {
    /// "PC-1500", "PC-1500A" or "PC-1600" (canonical spelling; the ROM
    /// suffix is split off into `romVariant`).
    std::string model;
    /// The PC-1600 is a separate machine (Core/PC1600/), applied by
    /// Core/PC1600/PC1600PresetLoader.cpp, not applyPC1500Preset().
    bool isPC1600() const { return model == "PC-1600"; }
    // PC-1500 / PC-1500A only. Whoever calls applyPC1500Preset() runs it
    // against a PC1500Machine already constructed with this variant (fixed
    // at construction -- see PC1500Memory.hpp).
    PC1500Variant variant = PC1500Variant::PC1500A;
    // The ROM suffix of `model: NAME[:ROM]`, defaulted. PC-1600: "new" or
    // "old". PC-1500: "A01", "A03" or "A04" (default A04); the PC-1500A runs
    // A04 only. WHERE the ROM file lives is the caller's concern (a CLI's
    // `roms/` vs the GUI's bundled resources).
    std::string romVariant;
    // `keys:`/`program:` blocks, in file order.
    std::vector<PresetSection> sections;
    // The plotter: "" (none), "ce150" (`plotter: CE-150`, any model; on the
    // PC-1600 it sits on the LH5803 side, MODE 1) or "ce1600p" (`plotter:
    // CE-1600P`, PC-1600 only). Attached before the cold boot so the ROM
    // sees it.
    std::string plotter;
    // The interface: "" (none) or "ce158" (`interface: CE-158`). On a
    // PC-1600 it sits on the LH5803 side and can't be combined with the
    // CE-1600P.
    std::string interfaceName;
    // PC-1600 only: "new" or "old" -- the CE-1600P ROM version, from
    // `plotter: CE-1600P:old` (default "new", also when there is no plotter).
    // Independent of `romVariant`; the CE-1600F in the same box follows it.
    std::string ce1600pRomVariant = "new";

    // PC-1600 only, and only with the CE-1600P (the CE-1600F comes with it).
    // `floppy: <name>` names a saved disk by its `disk-name` (bundled first,
    // then the save folder -- Connector/FloppyImageFile.hpp);
    // `floppy-file: <path>` names a `.floppy.yaml` file instead. An optional
    // `,A` / `,B` suffix picks the side facing the head (`floppySide`). At
    // most one of the two is set; both empty = no disk, as the GUI's
    // "–empty–".
    std::string floppy;
    std::string floppyFile;
    int floppySide = 0;  // 0 = side A (default), 1 = side B

    // PC-1600 only: `host-drive: <dir>` mounts that folder as drive S3:
    // (PC1600HostDriveCard), attached before the cold boot. Resolved like
    // every path. Empty = no host drive. docs/PC1600-Host-Drive.md.
    std::string hostDrive;

    // The memory module in each slot (the PC-1500/1500A has `slot-1` only),
    // a docs/Memory-Card-Definition-Format.md definition:
    //  * `slot-N-file: <path>` -- a definition FILE, resolved.
    //  * `slot-N: <module-name>` -- a bundled or saved module by its
    //    `module-name:`, stored verbatim; the loader looks it up
    //    (Core/Connector/MemoryCardCatalog.hpp's resolveModuleSpecByName()).
    // At most one of the pair is set; both empty = an empty slot. The loader
    // turns it into a card via Core/Connector/SoftwareDefinedCard.hpp.
    std::string slot1ModuleSpecFile;
    std::string slot1ModuleSpecName;
    std::string slot2ModuleSpecFile;
    std::string slot2ModuleSpecName;

    // `bus-rom:` -- ROM files plugged into the 60-pin bus before power-on,
    // in front of every other card there, so they shadow a bundled ROM at
    // the same place (developing a ROM extension, docs/Debugger.md).
    std::vector<PresetBusRom> busRoms;

    /// Not parsed: set by the caller. The loaders then stop once the machine
    /// is armed (model, modules, peripherals, bus ROMs) -- no reset, no boot
    /// run, no `keys:` / `program:` -- for a debugger that runs the boot
    /// itself (`debug: boot: debug`).
    bool armOnly = false;

    // `debug:` -- the debugger's attach settings for a project preset
    // (Core/Preset/PresetDebugBlock.hpp), paths already resolved. Null when
    // the preset has none. Loading the preset ignores it.
    YamlNode debug;
};

/// Parses the preset file at `path` into `out`. File paths inside the
/// preset are resolved relative to `path`'s own directory; a `program:`
/// file is read and classified here. Returns false and fills `error` (including a line
/// number when available) on any parse failure or unsupported field; a
/// preset is parsed all-or-nothing, so a bad preset fails before the
/// machine is even started.
bool parsePresetFile(const std::string& path, PresetFile* out, std::string* error);

/// A path in a preset: `~` / `~/...` is the home directory, anything else
/// relative is relative to the preset's directory `dir`.
std::string resolvePath(const std::filesystem::path& dir, const std::string& value);

/// A number anywhere in a preset: `&`, `0x` or `$` makes it hex, otherwise
/// decimal. The whole value must be the number.
bool parseNumber(const std::string& value, uint32_t* out);
