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
/// order (runPresetSections()), running each `keys:` block and loading
/// each `program:` block. The step verbs and program forms are described
/// in docs/User-Guide.md chapter 8. PC-1600 specifics: `type:` goes
/// through PC1600BasicTyper; a BASIC `file:` is poked into the program
/// area MODE and TITLE select (PC1600BasicLoader); typed BASIC needs PRO
/// mode and reports lines the editor didn't store in `rejectedBasicLines`;
/// a trace captures both CPUs into one file, tagged by cpuId.
///
/// The directories are the caller's, because where they are depends on
/// the environment:
///   - `traceDir`: where `trace:` and `screenshot:` write (CLI ".", GUI
///     its trace directory setting).
///   - `moduleDir`, then `extraModuleDirs` in order: searched for a
///     `slot-N: <module-name>` (MemoryCardCatalog). The CLI passes its
///     `--modules-dir`s, the GUI its bundled cards and then its save
///     folder. A missing extra directory is skipped. `slot-N-file:` is
///     resolved by the parser and never looks here.
///   - `romDirs`: where a `plotter:`'s bundled ROM is found by name
///     (BundledRomCatalog); the GUI passes its resources folder, the CLI
///     `roms/`. A preset with a plotter but no matching ROM fails.
/// `onArmed` fires right before the cold boot, once cards/plotter are
/// attached but the machine is still powered off -- see PresetArmedFn.
/// `onSaveAs` fires for each `saveas:` step -- see PresetSaveAsFn.
PresetLoadResult applyPC1600Preset(PC1600Machine& machine, const PresetFile& preset,
                                   const PresetLogFn& log = {},
                                   const std::string& traceDir = ".",
                                   const std::string& moduleDir = ".",
                                   const PresetBootedFn& onBooted = {},
                                   const std::vector<std::string>& romDirs = {},
                                   const std::vector<std::string>& extraModuleDirs = {},
                                   const PresetArmedFn& onArmed = {},
                                   const PresetSaveAsFn& onSaveAs = {});
