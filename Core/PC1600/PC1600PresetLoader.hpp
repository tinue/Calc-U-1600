#pragma once
#include <string>
#include <vector>

#include "../Preset/PresetFile.hpp"
#include "../Preset/PresetRunner.hpp"

class PC1600Machine;

/// Applies a `model: PC-1600` preset (already parsed via parsePresetFile,
/// `preset.isPC1600 == true`) to `machine`. The machine must ALREADY have
/// its ROM set loaded (the PC-1600 ROM set is fixed and environment-
/// specific to locate -- the caller loads it, unlike the PC-1500 loader
/// which resolves a single ROM path itself).
///
/// Steps, in order: plug the preset's `slot-1:` / `slot-2:` modules into
/// the two memory-slot connectors (before reset, so the boot ROM's own
/// memory sizing sees them); ALL RESET; run past the boot sequence (fixed
/// settle + a BUSY-symbol idle poll); then walk `preset.sections` in file
/// order (runPresetSections(), Core/Preset/PresetRunner.hpp), running each
/// `keys:` block and loading each `program:` block.
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
///   - `screenshot:` -- `- screenshot: name.png` writes a PNG of the LCD
///     graphics area (no status strip; Core/Display/LcdScreenshot.hpp,
///     89 x 18 mm at 600 DPI) into `traceDir`, overwriting.
///   - `syncclock:` -- re-seeds the RTC from the host's local time
///     (Core/HostClock.hpp). The load runs flat out and leaves the clock
///     ahead of real time -- make it the last step.
///   - `saveas:` -- `- saveas: live|template slot-1:<name>` / `slot-2:` /
///     `floppy:` saves the live card in slot 1/2, or the live floppy, under
///     `<name>` via `onSaveAs`; a no-op (logged) if `onSaveAs` is unset.
///
/// A `program:` block is one of (PresetProgram, Core/Preset/PresetFile.hpp):
///   - a BASIC `file:` -- a listing or tokenized BASIC, poked straight into
///     the program area the MODE and TITLE select; the preset must have
///     run `NEW0` in PRO mode first.
///   - typed BASIC (`text: |`, or `file:` + `typed: true`) -- typed in
///     through the ROM's line editor, which only stores lines in PRO mode,
///     so the loader ASSUMES the machine is already there and leaves it in
///     PRO mode. A line that is over-length, or that the editor doesn't
///     store (detected via the BASPRG_END pointer), is reported in
///     `rejectedBasicLines`.
///   - a machine-code `file:` -- placed by MODE, TITLE and the address, as
///     Load Machine Code does. The load address and byte count come from a
///     16-byte PC-1600 header when the file has one; `address:` /
///     `length:` override their header field, and a headerless file needs
///     `address:`. Reserving the target region (`NEW "S0:",n`) is the
///     preset's job. A non-zero auto-run address in the header makes the
///     loader type the `CALL` and wait for the interpreter to return; this
///     needs the machine in RUN mode.
///
/// `traceDir` is where a `- trace: name.bin` step writes -- WHERE trace
/// files live is environment-specific, not the preset's concern (the CLI
/// passes ".", the GUI passes `AppSettings.traceDirectory()`). The
/// preset's filename is appended verbatim. `screenshot:` writes there
/// too. A preset with neither step never touches it.
///
/// `moduleDir` is the directory searched first for a
/// `slot-N: <module-name>` reference (a bundled/standard module
/// named by its `module-name:`), via Core/Connector/MemoryCardCatalog.hpp
/// -- the CLI passes its `--modules-dir` (default `Qt6/resources/cards`),
/// the GUI its bundled resource path. `extraModuleDirs` are additional
/// directories searched, in order, when `moduleDir` misses -- the GUI
/// passes its iCloud-Drive `BatteryCards/` folder so a preset can name a
/// user's saved battery-card instance; the CLI passes any repeated
/// `--modules-dir`. A missing/unreadable extra directory is skipped
/// silently. Only consulted for the name form; `slot-N-file: <path>`
/// is resolved by the parser and never looks here.
/// `romDirs` is where the plotter's bundled ROM(s) live, needed only when
/// the preset has `plotter: CE-150`/`plotter: CE-1600P` (the plotter is
/// attached before the cold boot so the ROM detects it) -- resolved by
/// name via Core/Resources/BundledRomCatalog.hpp, mirroring how `moduleDir`
/// is resolved by Core/Connector/MemoryCardCatalog.hpp. WHERE those
/// directories are is environment-specific -- the GUI passes its bundled
/// resources folder, the CLI its `roms/` directory -- so the caller
/// supplies the directory, never a path to a specific file. A preset that
/// asks for the plotter without a matching directory fails with a clear
/// message; a preset with no `plotter:` never touches `romDirs`.
/// `onArmed` fires right before the cold boot, once cards/plotter are
/// attached but the machine is still powered off -- see PresetArmedFn.
/// `onSaveAs` fires for each `saveas:` step encountered while walking the
/// preset's sections -- see PresetSaveAsFn.
PresetLoadResult applyPC1600Preset(PC1600Machine& machine, const PresetFile& preset,
                                   const PresetLogFn& log = {},
                                   const std::string& traceDir = ".",
                                   const std::string& moduleDir = ".",
                                   const PresetBootedFn& onBooted = {},
                                   const std::vector<std::string>& romDirs = {},
                                   const std::vector<std::string>& extraModuleDirs = {},
                                   const PresetArmedFn& onArmed = {},
                                   const PresetSaveAsFn& onSaveAs = {});
