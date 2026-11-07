# Names and descriptions for presets, cards and disks

> **Status (2026-11-07): implemented** (4e2c58a … this commit). Differences from this plan:
> - **Core:** `preset-name:` / `description:` values go through the parser's usual quote and comment handling (`splitKeyValue`), so no special quote rule was needed for Decisions.md. The headless `saveas: floppy:` (Core/Preset/PresetMedia.hpp) writes no description: it has only the image, not the disk's source file.
> - **Shared:** `FloppyBay::DiskLists` entries carry the disk's description (like `ModuleBay::ModuleEntry`). Card instances keep the description for free (a save is spliced into the source file). `HostDirs` got a third field, `bundledExamples` (default `<bundle>/examples`; the web's is `/presets`).
> - **Web:** the menu is still **File ▸ Load Preset ▸** (examples, then Upload Preset…), not "Examples"; the engine gives `presetExamples()` and `settingsState().presetChoices`. `?preset=` still takes a path under `presets/`.
> - **Settings (both):** the Qt row lost its Clear button; "(none)" / "None" is the first item of the list.

## Context

Web ▸ Settings ▸ Default presets shows labels like "Towers of Hanoi (PC-1600)". They
come from `Web/presets.json`, a hand-kept list used only by the web build
(`Web/build.js` copies it to `presets/manifest.json`; `CalcU1600.jsx:217` builds
File ▸ Examples from it, `Dialogs.jsx:178` the Settings selects). The preset files
have no name at all. Qt6 has none of this: its default preset is a raw path field
(`SettingsDialog.cpp:166`) and Load Preset is a file dialog. The closed web select
crops the long label ("Towers of Hanoi (PC-16…").

Goal: the label belongs in the file itself, as a **short name** (shown once
something is selected; unique and uncropped) plus a **long description** (shown in
lists, menus and tooltips). This works the same in both frontends, and cards and
disks get the same description field.

Decisions (from the user):
- Preset keys `preset-name:` (short) + `description:` (long), matching
  `module-name:` / `disk-name:`.
- Both are optional; when missing, the file name stands in. A test makes them
  required for every preset in `examples/`.
- Cards and disks get an optional one-line `description:`, shown as a tooltip.
  Name & Save copies it from the template to the new instance.
- Qt bundles the same curated example list as the web: File ▸ Examples, and a pick
  list in Settings ▸ Default presets.

## Design

### 1. Core — the format
- `Core/Preset/PresetFile.{hpp,cpp}`: add `preset-name` and `description` to
  `kKeys`, as inline values only. The value is the rest of the line, with one
  optional pair of surrounding `"` stripped. That matches the card/disk files and
  differs on purpose from `type:` (record this in Decisions.md). New fields:
  `PresetFile::presetName`, `PresetFile::description`.
- Add a cheap header read, `bool readPresetHeader(path, PresetHeader*, error)`,
  returning `{model, presetName, description}`. It scans only the top-level
  `format-version` / `model` / `preset-name` / `description` lines and does not
  resolve files or steps, so listing presets doesn't parse everything. It reuses
  the same `readLines` and `splitKeyValue`. The fallback name is the file stem
  (`hanoi`); the fallback description is empty. This fallback lives in one helper
  so both frontends get it.
- Cards: `Core/Connector/MemoryCardDefinition.hpp:1359`: add `description` to the
  allowed keys → `MemoryCardDefinition::description`; carry it into
  `MemoryCardCatalogEntry` (`MemoryCardCatalog.hpp:26`). The existing `notes:`
  (free-form, multi-line) stays as it is.
- Disks: `Core/Connector/FloppyImageFile.hpp:76`: add `description` to
  `requireOnlyKeys` → `FloppyFile::description`; the writer (line ~109) emits it
  when it isn't empty.
- Format versions stay at 1: the new keys are optional, and pre-1.0 is fine with
  that.

### 2. Shared — one catalogue for both frontends
- **The curated lists move to `examples/bundled.yaml`** (replaces
  `Web/presets.json`). It holds two separate lists:
  ```yaml
  defaults:            # Settings ▸ Default presets pick list (per model)
    - file: startup/default-pc1500.pc1500
    - file: startup/default-pc1500a.pc1500a
    - file: startup/default-pc1600.pc1600
  examples:            # File ▸ Examples (presets to load at runtime)
    - group: BASIC
      file: basic/hanoi.pc1600
    - group: Plotter
      file: plotter/ce150_demo.pc1500a
    …
  ```
  A preset in `defaults` is not offered under File ▸ Examples (the web menu's
  "Startup" group goes away). The Settings list offers only `defaults` entries.
  Model, name and description come from each file itself, so they can't drift out
  of sync. Shared reads the file with `Core/Yaml.hpp`; `build.js` and CMake only
  need the `file:` lines from both lists.
- New `Shared/PresetCatalog.{hpp,cpp}`: `defaults(model)` and `examples()` →
  `{group, ref, model, name, description}`, read from
  `<bundledResourcesDir>/examples/bundled.yaml` plus each file's
  `readPresetHeader`; and `PresetHeader describe(path)` for any file (the user's own
  default preset, uploads). It refuses a file that is in both lists or whose
  `model:` doesn't parse; the test checks this too.
- **Stable reference for bundled presets in Settings:** `examples:basic/hanoi.pc1600`.
  Shared resolves it against the bundled examples folder (`AppPaths`), so it
  survives app moves and updates in Qt. The web uses the same form instead of
  `/presets/...`; old web values simply no longer resolve (pre-1.0).
  `AppSession::loadDefaultPresetJob` / `loadPresetJob` accept the form.
- `ModuleBay::ModuleEntry` (`Shared/ModuleBay.hpp:46`) and the `FloppyBay` lists
  gain `description` (FloppyBay's `templates`/`instances` become entries rather than
  bare strings). `MediaLibrary::MediaItem` gains it too (Saved Media dialog).
- Name & Save (the `MediaSave.hpp` pipeline / `CardYamlDump`): copy the source's
  `description` into the saved card or disk.
- Validation helper `presetNameProblems(entries)`: names unique per model,
  ≤ 18 characters, no model suffix needed (the Settings lists are already per
  model). Used by the test.

### 3. Qt6
- CMake (`Qt6/CMakeLists.txt` ~175, next to the cards/ROMs copy): copy
  `examples/bundled.yaml` and the folders of the listed files into
  `Resources/examples` (loose, like the cards). The folder list comes from
  `file(STRINGS … REGEX "file:")`.
- `MainWindow.cpp:839` File menu: **Examples ▸** submenu from
  `PresetCatalog::examples()` (no default presets), grouped like the web's. Item
  text = description, falling back to the name. It runs the existing load-preset
  path.
- `SettingsDialog.cpp` default-preset rows: a combo instead of the path field.
  Entries: "None", `PresetCatalog::defaults(model)`, and the current custom file
  if one is set. Each list item shows the **description**, the closed combo shows
  the **short name** (a small `NamedComboBox`: paint the current item's
  `Qt::UserRole+1` short name; tooltip = description + path). "Choose…" still picks
  any file, and that file's name/description come from `PresetCatalog::describe`.
- Control-bar module and floppy pickers (`ControlBar.cpp:31 fillPicker`, 148):
  `setItemData(i, description, Qt::ToolTipRole)`, and the combo's own tooltip
  follows the current item.

### 4. Web
- `Web/build.js`: read `examples/bundled.yaml` (simple `file:` line scan), copy the
  folders as now, and drop `manifest.json`. `Web/presets.json` is removed.
- `bindings.cpp`: `presetCatalog()` → the Shared list. The engine already has the
  files at `/presets`, and that becomes the bundled examples folder for
  `AppPaths`. Module/floppy lists pass `description` through.
- `CalcU1600.jsx:217` Examples menu: `examples()` from the engine (no Startup
  group), labelled with the description. The Settings combos use `defaults(model)`.
- `Dialogs.jsx:178` Settings: replace the native `<select>` with a small
  `NamedCombo` built on `MenuList` (`MenuBar.jsx:11`). The button shows the short
  name, the open list shows descriptions. A native select can't show different
  text open and closed.
- `ControlBar.jsx:35` module/disk selects: `title` = description on each option and
  on the select itself.

### 5. Content and docs
- Add `preset-name:` and `description:` to every preset in both lists of `bundled.yaml`,
  plus the other `examples/` presets. Short names like "Hanoi", "Lissajous CE-150",
  "Lissajous CE-1600P"; descriptions take over today's titles.
- Add `description:` to the bundled cards (`Qt6/resources/cards/*.card.yaml`) and
  disks ("Formatted", "Blank"), from their header comments.
- Docs: the User-Guide preset key table and the card/disk format sections;
  `docs/developer/Web-Client.md` (presets.json → bundled.yaml);
  `docs/background/Decisions.md` (the quote rule for these two keys; `examples:`
  refs); `examples/README.md` mentions bundled.yaml. Commit the finished plan as
  `docs/background/plans/Media-Names-Plan.md` (+ a row in the plans README).

## Phases (each one committed to dev-0.8.0)
1. Core keys + `readPresetHeader` + card/disk `description`, with tests.
2. Shared `PresetCatalog`, `examples:` refs, descriptions in bays/MediaLibrary,
   Name & Save copy; `bundled.yaml` + example content + the examples test.
3. Qt6: bundling, Examples menu, Settings `NamedComboBox`, picker tooltips.
4. Web: build.js, bindings, Examples menu, `NamedCombo`, option titles; Playwright
   update (`Web/tests/offline.spec.js:61` clicks the Hanoi example by text).
5. Docs + Decisions + plan doc.

## Verification
- `tools/run_tests.sh`. New tests:
  - Core: preset-name/description parsing, the quoted form, the fallback, an
    unknown-key error unchanged.
  - Core: a card/disk round trip with `description`.
  - Shared: `PresetCatalog` over `examples/bundled.yaml`. Every listed file
    exists, parses, has both keys, and passes `presetNameProblems` (per list).
    No file is in both lists.
  - Shared: an `examples:` ref resolves and loads as the default preset.
  - Shared: Name & Save keeps the description.
- `tools/build_wasm_check.sh`; Qt app builds.
- Qt, by hand (`-ApplePersistenceIgnoreState YES`, quit via calcu1600/quit):
  - File ▸ Examples ▸ Hanoi loads; the startup presets are not in the menu.
  - Settings: the PC-1600 list shows descriptions, the closed combo shows "Hanoi";
    restart → it loads as the default.
  - The module picker tooltip shows the CE-161 description.
- Web: `Web/run-tests.sh` (Playwright).
- Web, by hand in Chrome: the Settings combo shows the short name uncropped and the
  descriptions when open; the Examples menu still works; reload keeps the default.
