#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../PC1500/PresetFile.hpp"

class PC1600Machine;

/// Optional per-step progress sink -- one already-formatted line per call
/// (no trailing newline), same convention as PC1500PresetLoader's
/// PresetLogFn.
using PC1600PresetLogFn = std::function<void(const std::string&)>;

/// Optional callback fired exactly once, right after the machine has been
/// slot-populated, ALL RESET, and settled past its boot sequence -- i.e.
/// once `machine` is a fully live, running PC-1600 -- but *before* any of
/// the preset's own `keys:`/`program:` steps start executing. A GUI can
/// use this to swap the visible/active machine over at this point rather
/// than waiting for the whole (possibly many-seconds-long) script to
/// finish first: the boot itself is fast, so the model switch becomes
/// visible immediately and the preset's own keystrokes/program typing
/// then play out live on the already-switched-to machine, same as a
/// person driving it by hand would see. Left unset (the default) costs
/// nothing and changes no behavior.
using PC1600PresetBootedFn = std::function<void()>;

struct PC1600PresetLoadResult {
    bool ok = false;
    std::string error;
    /// From a `program:` (basic-text) block: source lines the PC-1600
    /// editor didn't store -- over-length lines (caught up front) and
    /// lines that were typed but didn't advance BASPRG_END (usually
    /// because the machine wasn't in PRO mode). Non-empty implies
    /// `ok == false`. Mirrors PresetLoadResult::rejectedBasicLines.
    std::vector<std::string> rejectedBasicLines;
    /// The module the preset plugged into each slot, for the GUI to label
    /// its control-bar buttons with -- a software-defined module's
    /// `module-name:` (both `modulespec:`/`modulespecfile:` forms) or a
    /// built-in `- module:` name verbatim. Empty for an empty slot.
    std::string slot1ModuleLabel;
    std::string slot2ModuleLabel;
    /// The on-disk file a `modulespec:`/`modulespecfile:` reference
    /// resolved to, if any -- empty for a built-in `- module: <name>` (not
    /// file-backed) or an empty slot. Lets the GUI tell a bundled
    /// read-only template apart from a real saved battery-card instance
    /// (a file under the writable instance directory) so it can decide
    /// whether the attached module should autosave.
    std::string slot1ResolvedPath;
    std::string slot2ResolvedPath;
    /// True when the preset had `plotter: ce150` and the CE-150 was attached
    /// to the LH5803 side. (`plotter: ce1600p` is reported via
    /// `machine.ce1600pAttached()` instead -- no result field for it yet.)
    bool ce150Attached = false;
    /// The preset's `floppy:` name, verbatim -- empty if the key was
    /// absent (the CE-1600F still got the usual auto-inserted blank disk,
    /// per its union attach with `plotter: ce1600p`; see
    /// PC1600Machine::attachCE1600P()). Mirrors slot1ModuleLabel's shape,
    /// for the GUI (FloppyDiskManager::syncFromPresetLoad()) to resync its
    /// disk-picker combo without re-attaching anything.
    std::string floppyImageLabel;
    /// The on-disk `*.floppy.yaml` file `floppyImageLabel` resolved
    /// to, if any -- empty when `floppyImageLabel` is empty. Mirrors
    /// slot1ResolvedPath's shape/purpose (telling a bundled template apart
    /// from a real saved user instance).
    std::string floppyResolvedPath;
};

/// Optional callback fired exactly once, right after the machine has been
/// slot-populated and the plotter (if any) attached, but *before* ALL
/// RESET -- i.e. `machine` is fully "armed" (model, cards, plotter all
/// wired) yet still powered off. `armedSoFar` is the in-progress result:
/// `slot1ModuleLabel`/`slot2ModuleLabel`/`ce150Attached` are already final
/// at this point (nothing after boot changes what's plugged in), so a GUI
/// can use this to resync its slot selectors and plotter-paper visibility
/// and repaint the armed-but-off machine before the (possibly many-
/// seconds-long) boot and preset script run. Left unset (the default)
/// costs nothing and changes no behavior.
using PC1600PresetArmedFn = std::function<void(const PC1600PresetLoadResult& armedSoFar)>;

/// Applies a `model: PC-1600` preset (already parsed via parsePresetFile,
/// `preset.isPC1600 == true`) to `machine`. The machine must ALREADY have
/// its ROM set loaded (the PC-1600 ROM set is fixed and environment-
/// specific to locate -- the caller loads it, unlike the PC-1500 loader
/// which resolves a single ROM path itself).
///
/// Steps, in order: plug `preset.slot1Module` / `preset.slot2Module` into
/// the two memory-slot connectors (before reset, so the boot ROM's own
/// memory sizing sees them); ALL RESET; run past the boot sequence (fixed
/// settle + a BUSY-symbol idle poll); then walk `preset.sections` in file
/// order, running each `keys:` block and typing in each `program:` block.
///
/// A `keys:` step is one of:
///   - `key:` -- one named PC-1600 key, or `break`/`on` for the ON key.
///     `key: mode` toggles RUN<->PRO.
///   - `type:` -- the line's characters sent as keystrokes (case-
///     sensitive: lowercase via a SHIFT-tap), then ENTER. Punctuation and
///     the digit-row second legends go via SHIFT + a base key. See
///     Core/PC1600/PC1600BasicTyper.
///   - `wait:` -- idle N seconds.
///   - `trace:` -- start/stop a CPU instruction trace to a file under
///     `traceDir` (`- trace: name.bin` starts, `- trace: off` stops; an
///     open trace is auto-closed when the preset finishes). Both CPUs'
///     rings are captured into one file, tagged by cpuId.
///
/// A `program:` block is one of:
///   - `format: basic-text` / `format: basic-binary` -- a BASIC program.
///     `basic-text` lines are typed in through the ROM's line editor,
///     which only stores lines in PRO mode, so the loader ASSUMES the
///     machine is already there -- put a `key: mode` step before the
///     block, and drive the machine afterward yourself (the loader leaves
///     it in PRO mode). A line that is over-length, or that the editor
///     doesn't store (e.g. not in PRO mode -- detected via the BASPRG_END
///     pointer), is reported in `rejectedBasicLines`.
///   - `format: binary` -- a machine-language block, loaded LINEARLY into
///     the one slot named by `slot: S0|S1|S2` (S0 = internal RAM
///     $C000-$FFFF, S1/S2 = the $8000-$BFFF memory-slot window). The load
///     address and byte count come from a 16-byte PC-1600 machine-language
///     header (magic FF 10 00 00, type 0x10) when the file has one;
///     `address:` / `length:` in the preset each override their header
///     field, and both are required when the file has no header. Reserving
///     the target region (`NEW &addr`) is the preset's job -- do it with
///     `keys:` steps before the block. If the header carries a non-zero
///     auto-run address the loader then types `CALL &<addr>` and waits for
///     the interpreter to return; this needs the machine in RUN mode (the
///     default after boot -- if a preceding block went to PRO, `key: mode`
///     back before the `binary` block).
///
/// `traceDir` is where a `- trace: name.bin` step writes -- WHERE trace
/// files live is environment-specific, not the preset's concern (the CLI
/// passes ".", the GUI passes `AppSettings.traceDirectory()`). The
/// preset's filename is appended verbatim. A preset with no `trace:` step
/// never touches it.
///
/// `moduleDir` is the directory searched first for a
/// `- modulespec: <module-name>` slot reference (a bundled/standard module
/// named by its `module-name:`), via Core/Connector/MemoryCardCatalog.hpp
/// -- the CLI passes its `--modules-dir` (default `Calc-U-1600/Resources`),
/// the GUI its bundled resource path. `extraModuleDirs` are additional
/// directories searched, in order, when `moduleDir` misses -- the GUI
/// passes its iCloud-Drive `BatteryCards/` folder so a preset can name a
/// user's saved battery-card instance; the CLI passes any repeated
/// `--modules-dir`. A missing/unreadable extra directory is skipped
/// silently. Only consulted for the name form; `- modulespecfile: <path>`
/// is resolved by the parser and never looks here.
/// `romDirs` is where the plotter's bundled ROM(s) live, needed only when
/// the preset has `plotter: ce150`/`plotter: ce1600p` (the plotter is
/// attached before the cold boot so the ROM detects it) -- resolved by
/// name via Core/Resources/BundledRomCatalog.hpp, mirroring how `moduleDir`
/// is resolved by Core/Connector/MemoryCardCatalog.hpp. WHERE those
/// directories are is environment-specific -- the GUI passes its bundled
/// resources folder, the CLI its `roms/` directory -- so the caller
/// supplies the directory, never a path to a specific file. A preset that
/// asks for the plotter without a matching directory fails with a clear
/// message; a preset with no `plotter:` never touches `romDirs`.
/// `onArmed` fires right before the cold boot, once cards/plotter are
/// attached but the machine is still powered off -- see PC1600PresetArmedFn.
PC1600PresetLoadResult applyPC1600Preset(PC1600Machine& machine, const PresetFile& preset,
                                         const PC1600PresetLogFn& log = {},
                                         const std::string& traceDir = ".",
                                         const std::string& moduleDir = ".",
                                         const PC1600PresetBootedFn& onBooted = {},
                                         const std::vector<std::string>& romDirs = {},
                                         const std::vector<std::string>& extraModuleDirs = {},
                                         const PC1600PresetArmedFn& onArmed = {});
