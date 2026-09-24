#pragma once
#include <string>
#include <vector>

#include "../Preset/PresetFile.hpp"
#include "../Preset/PresetRunner.hpp"

class PC1500Machine;

/// Applies `preset` (already parsed via parsePresetFile) to `machine`:
/// loads firmware, resets, steps past the boot sequence, then walks
/// `preset.sections` in file order (runPresetSections(),
/// Core/Preset/PresetRunner.hpp), running each `keys:` block's steps or
/// loading+applying each `program:` block as it's reached -- a preset can
/// interleave any number of `keys:`/`program:` blocks (e.g. install a
/// loader program, run it via a `keys:` step, then load a second payload
/// program) -- see PresetFile.hpp's top-of-file comment. Deliberately out
/// of scope: rom-modules and check steps (parsePresetFile already rejects
/// preset files using those).
///
/// `preset.romVariant` (e.g. "A04") says *which* ROM to run, not *where*
/// its file lives -- `romDirs` is where to look for it (and for the CE-150
/// ROM, if `plotter: ce150`), resolved by name via
/// Core/Resources/BundledRomCatalog.hpp: the CLI passes its `roms/`
/// directory, the GUI its bundled resources folder. A preset that needs a
/// ROM `romDirs` doesn't contain fails with a clear message.
/// `traceDir` is the directory a `- trace: name.bin` step (see
/// PresetFile.hpp -- a port of Calc-U-59's `Trace:` directive) writes
/// its output file into. Like `romPathOverride`, WHERE trace files live
/// is environment-specific and not the preset's concern: the CLI passes
/// "." (cwd, same convention as its `roms/`), the GUI passes
/// `AppSettings.traceDirectory()`. The preset's filename is appended to
/// it verbatim (the parser has already rejected a path separator in it).
/// A `- screenshot: name.png` step writes a PNG of the LCD dot matrix
/// (Core/Display/LcdScreenshot.hpp, 104 x 5 mm at 600 DPI) into the same
/// directory, overwriting. A preset with neither step never touches
/// `traceDir`.
///
/// A `- syncclock:` step re-seeds the RTC from the host's local time at
/// that point (Core/HostClock.hpp). The load itself runs flat out, which
/// leaves the clock ahead of real time -- make it the last step.
///
/// A `- saveas: s1:<name>` step saves the live expansion-slot card under
/// `<name>` via `onSaveAs`; a no-op (logged) if `onSaveAs` is unset.
///
/// `moduleDir` is the directory searched first for a
/// `- modulespec: <module-name>` memory-expansion reference (a
/// bundled/standard module named by its `module-name:`), via
/// Core/Connector/MemoryCardCatalog.hpp. Same environment-specific split as
/// the others: the CLI passes its `--modules-dir` (default
/// `Calc-U-1600/Resources`), the GUI passes its bundled resource path.
/// `extraModuleDirs` are additional directories searched, in order, when
/// `moduleDir` has no match -- the GUI passes its iCloud-Drive
/// `BatteryCards/` folder here so a preset can name a user's saved
/// battery-card instance; the CLI passes any repeated `--modules-dir`. A
/// missing/unreadable extra directory is skipped silently. Only consulted
/// when a preset actually uses the name form; a `- modulespecfile: <path>`
/// reference is resolved by the parser and never looks here.
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
