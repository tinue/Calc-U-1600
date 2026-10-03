#pragma once
#include <string>
#include <vector>

#include "../Preset/PresetFile.hpp"
#include "../Preset/PresetRunner.hpp"

class PC1500Machine;

/// Applies `preset` (already parsed via parsePresetFile) to `machine`:
/// loads firmware, resets, steps past the boot sequence, then walks
/// `preset.sections` in file order (runPresetSections()), running each
/// `keys:` block and loading each `program:` block. The step verbs and
/// program forms are described in docs/User-Guide.md chapter 8.
///
/// The directories are the caller's, because where they are depends on
/// the environment:
///   - `romDirs`: where the ROM `preset.romVariant` names (and the CE-150
///     ROM for `plotter: CE-150`) is found by name (BundledRomCatalog);
///     the CLI passes `roms/`, the GUI its resources folder. A preset
///     whose ROM isn't there fails.
///   - `traceDir`: where `trace:` and `screenshot:` write (CLI ".", GUI
///     its trace directory setting).
///   - `moduleDir`, then `extraModuleDirs` in order: searched for a
///     `slot-1: <module-name>` (MemoryCardCatalog). The CLI passes its
///     `--modules-dir`s, the GUI its bundled cards and then its save
///     folder. A missing extra directory is skipped. `slot-N-file:` is
///     resolved by the parser and never looks here.
/// `onArmed` fires right before reset(), once the module/plotter are
/// attached but the machine is still powered off -- see PresetArmedFn.
/// `onSaveAs` fires for each `saveas:` step -- see PresetSaveAsFn.
PresetLoadResult applyPC1500Preset(PC1500Machine& machine, const PresetFile& preset,
                                   const PresetLogFn& log = {},
                                   const std::string& traceDir = ".",
                                   const std::string& moduleDir = ".",
                                   const PresetBootedFn& onBooted = {},
                                   const std::vector<std::string>& romDirs = {},
                                   const std::vector<std::string>& extraModuleDirs = {},
                                   const PresetArmedFn& onArmed = {},
                                   const PresetSaveAsFn& onSaveAs = {});
