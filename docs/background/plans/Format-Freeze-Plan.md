# Pre-1.0 format freeze: versions, `slot-N-rom:`, one `saveas:` pipeline

> **Status (2026-10-10): in progress.**


## Context

From 1.0.0 on, every change must stay backwards compatible, so anything that touches a file format goes in before 1.0. Agreed on 2026-10-10:

1. **Every public format carries a `format-version`.** Today only `.floppy.yaml` (`format-version: 1`) and TRACE.bin (magic + u16 version) have one. `.card.yaml` and presets have none. The version is required. Saved card instances that lack it **fail silently**: they drop out of the picker like any unreadable card, with no migration.
2. **`encoding: file` in `.card.yaml` is dropped** and replaced by a preset key `slot-N-rom: <bin>`. The key only overrides the ROM bytes of a card the preset already attaches with `slot-N:` or `slot-N-file:`, so the card definition (decode, banking) stays the source. A file of the wrong size is refused. The card's own placeholder ROM uses the existing `addressed-hex` shortcut `$0000: FF...`; no new `fill:` form is added.
3. **`saveas:` gets one model-neutral Core pipeline.** Today the GUI managers and `Core/PC1600/PC1600PresetMedia.hpp` each write the files, and `pc1500_cli` doesn't save at all.

Formats that stay unversioned: `.shots.yaml` (developer only), settings (internal), standard WAV files, and Sharp's own program headers.

Each phase is committed to `dev-0.8.0` and `tools/run_tests.sh` stays green. Phase 0 commits this plan as `docs/background/plans/Format-Freeze-Plan.md`, with a row in `docs/background/README.md`.

---

## Phase 1: `format-version` for cards (plus a shared helper)

- **Shared helper.** Add `requireFormatVersion(root, expected, "card", err)` next to the YAML reader (`Core/Yaml.hpp`). It returns "missing 'format-version'" when the key is absent and "unsupported card format-version N (this build reads 1)" when it doesn't match. Lift it from `floppy_detail::readHeader` (`Core/Connector/FloppyImageFile.hpp:77-88`), and switch the floppy reader to it with the same wording.
- **Card parser.** In `parseMemoryCardDefinition` (`Core/Connector/MemoryCardDefinition.hpp:1364`), add `"format-version"` to `requireOnlyKeys` (:1372) and check it first, before `module-name`. Add `kCardFormatVersion = 1`. All callers (catalogue, `makeSoftwareDefinedCard`, tests) go through this function.
- **Writers.** `spliceBatteryCardInstance` (`BatteryCardInstance.hpp:230`) already keeps unknown top-level lines, so the version line carries through every save. Add an assertion to the ce1638 round-trip test (`battery_card_instance_tests.cpp:369-395`).
- **Files to update:**
  - the 9 bundled cards in `Qt6/resources/cards/` (the line goes right above `module-name:`)
  - the 2 example cards (`examples/dwx/CE-1601M - Progs.card.yaml`, `examples/memory/memory-cards/pc1500-maxed-out.card.yaml`)
  - the inline card YAML in the tests: `memory_card_tests.cpp` (`kMinPrefix` plus about 9 literals), `battery_card_instance_tests.cpp`, `TestCards.hpp`. Leave the literals that are meant to be invalid as they are.
- **New tests:** a card with no version, a card with version 2, and a check that the floppy wording didn't change.
- **Docs:** add the key to `docs/Memory-Card-Definition-Format.md` §1 with a "Versioning" paragraph matching Floppy-Image-Format.md §4.

## Phase 2: `format-version` for presets

- **Parser.** In `Core/Preset/PresetFile.cpp` (`parsePresetFile`, :623), add a **pre-scan** of the indent-0 lines for `format-version` before the main loop. That way a preset from a newer format gets "unsupported preset format-version N (this build reads 1)" instead of an "unrecognized field" error. If the key is missing, the error is "'format-version' is required". Parse the value with the existing `parseNumber` (:29). Add the key to `kKeys` (:665), and have the dispatch chain skip it.
- **Files to update:** all 50 tracked presets (`examples/` 32, `dev/` 7, `vscode/` presets and templates 6, `docs/developer/screenshots/presets` 9), with `format-version: 1` as the first key. Also the preset generators:
  - `tools/dap_smoke.py`
  - `dev/tape-matrix/run.py`
  - `dev/loader-matrix/pc1500_loader_matrix.cpp`, `pc1600_loader_matrix.cpp`
  - `vscode/calcu1600-debug/templates/*`
- **Tests:** about 217 inline presets. Insert `format-version: 1\n` in front of the first `model:` with a script, and check the diff. Add tests for a missing version, version 2, and version 2 together with an unknown key (the version error wins).
- **Docs:**
  - docs/User-Guide.md §8.2 (the smallest preset now has two required keys), §8.3, and the §8.9 key table
  - the preset snippets in docs/Debugger.md
  - the header comment in `PresetFile.hpp:12-20`
  - Decisions.md "Preset format"
- **Check:** `tools/check_presets.sh` must pass on every tracked preset.

## Phase 3: drop `encoding: file`, add `slot-N-rom:`

**Removal** (`MemoryCardDefinition.hpp`):
- the `file` branch (:1179-1194) and `"path"` in the block keys (:1099)
- the `baseDir` parameter of `parseMemoryCardDefinition`, `parseRegion` and `parseInitialContent`
- the `FileIO.hpp` and `<filesystem>` includes there, and the `parent_path()` arguments plus their includes in `MemoryCardCatalog.hpp:50` and `SoftwareDefinedCard.hpp:351`
- the test `test_rom_from_a_sidecar_file` (`memory_card_tests.cpp:594`)

`encoding: file` then fails as an unknown encoding.

**New preset keys** `slot-1-rom:` and `slot-2-rom:` (`PresetFile.cpp`):
- The value is a path, resolved with `resolvePath` and stored as a path only, like `bus-rom:`. The bytes are read at apply time, so every clean start (Build & Load) reads them again.
- After the main loop:
  - `slot-N-rom` without `slot-N:` or `slot-N-file:` is an error: "'slot-1-rom:' needs a card in slot-1 ('slot-1:' or 'slot-1-file:')".
  - `slot-2-rom` on a PC-1500 or PC-1500A is rejected, as `slot-2` is today (:819).
- Fields go next to `slotNModuleSpecFile` in `PresetFile.hpp:214-225`.

**Applying the override:**
- Add `replaceRomContent(MemoryCardDefinition&, const std::vector<uint8_t>&, std::string* err)` in `MemoryCardDefinition.hpp`. Rules:
  - The card must have **exactly one region, and it must be `rom`**: `isRom()`, which excludes flash. A ROM-only card has nothing to autosave, and the instance splice handles single-region cards only anyway.
  - The file size must equal `bankCount * bankSize`, otherwise: "<path>: N bytes, the card's ROM is M bytes".
  - On success it sets `initialContentByBank[b]` from each slice.
- `makeSoftwareDefinedCard` (`SoftwareDefinedCard.hpp:339`) gets an optional ROM override, applied between parse and construction. `makePresetModuleCard` (`PresetRunner.cpp:20`) reads the file with `readWholeFile`. Errors carry the prefix "slot-N: ".
- The load result reports `slotNRomOverride`. A slot with an override must never be saved:
  - `saveas: … slot-N` on that slot is refused, in the Phase 4 pipeline.
  - `MemoryModuleManager::syncFromPresetLoad` marks the slot as not saveable, so no Name & Save and no autosave.

**Tests:**
- parse errors: no card in the slot, and `slot-2-rom` on a PC-1500
- a successful override on a PC-1600 slot and on a PC-1500 slot, checked by reading the bytes through the bus
- a size mismatch
- refused cards: a RAM card, a flash card (CE-163F), and a multi-region card
- re-reading: change the `.bin` between two applies
- `saveas:` refused on a slot with an override

**Docs and housekeeping:**
- docs/Debugger.md "ROM modules in a memory slot": the card file uses `$0000: FF...` and the preset uses `slot-1-file:` plus `slot-1-rom: build/module.bin`.
- `Memory-Card-Definition-Format.md`: drop `file` from the encodings (:254, :258-261, :303-309, §9), which also removes the contradictions there. Document that a ROM can be overridden from a preset.
- Decisions.md :639 "ROM extensions go in through preset keys (`bus-rom:`, `slot-N-rom:`)", plus a new entry saying `slot-N-rom` only overrides a ROM-only card and is never saved.
- Fix the stale comment at `SoftwareDefinedCard.hpp:21-24`.
- Remove the "Catalogue scans read `encoding: file` sidecar ROMs" item from TODO.md.
- User Guide §8.9 key table.
- CHANGELOG 0.8.0.

## Phase 4: one `saveas:` pipeline

- **New `Core/Connector/MediaSave.hpp`**, model-neutral:
  - `planCardSave(request, card image + bank count, source text, source module name, folders{bundled, saves})` → `{path, text}` or an error
  - `planFloppySave(request, disk image, folders)` → `{path, text}` or an error
  - `writeFileAtomically(path, text)`, moved out of `PC1600PresetMedia.hpp:26-42`
- **Checks folded into it** (today only the GUI makes them):
  - no `"` in the name
  - with no explicit `file:`: refuse a bundled name, and refuse a user-template name unless this is a template save
  - never overwrite an existing template file
  - refuse a slot with a ROM override (Phase 3)
  - the text comes from `formatBatteryCardInitialContentBlock` + `spliceBatteryCardInstance`, or from `formatFloppyFile`, and the path from `namedFileName()`
- **Callers:**
  - `PC1600PresetMedia.hpp` becomes a thin adapter that reads the images from `machine.memory()`, or is replaced by a generic `savePresetMedia(ExpansionCard*, floppy image*)`. Both slots are an `ExpansionCard*` on both models (`debugImage()` / `debugBankCount()`).
  - `pc1500_cli` gains `onArmed` + `onSaveAs` + `--save-dir` (`tools/pc1500_cli.cpp:167-180`), the same as `pc1600_cli.cpp:125-139`.
  - `MemoryModuleManager::saveSlotAs` and `FloppyDiskManager::saveDiskAs` call `plan*Save` and keep only what is GUI-specific: the interactive "already saved" check and `nameCollides`, the `tr()` wording, `atomicWriteFile`, autosave retargeting and signals.
- **Tests:**
  - by-name saves into a temporary save folder on both models
  - each refusal
  - a PC-1500 `saveas: live slot-1:<name>` through the shared path
  - the version line is kept
- **TODO.md:** remove "`saveas:` media is written by two pipelines". Trim "template-vs-instance written twice" to what is still open, the catalogue parse mode and classify.

## Phase 5: housekeeping

- **Decisions.md:**
  - Remove the stale entry "The slot-record layout stays additive… `batteryBacked`". `ExpansionCard` has no serialize, and battery cards are already named instances.
  - Add "Every public format carries a required `format-version`; a reader rejects unknown versions; a missing one fails like any unreadable file (saved cards without it drop out of the picker)".
- **TODO.md, "Guide screenshots write into the real saves folder":** `--shots` already isolates storage (`Qt6/app/main.cpp:113-122`). Confirm with `tools/make_screenshots.sh 05-modules` that the real saves folder stays untouched, then remove the entry.
- **CHANGELOG 0.8.0:** an entry for the format versions saying that saved cards and presets need `format-version: 1`.

---

## Verification

- `tools/run_tests.sh` after each phase, plus a `CoreTests` build in `build/`.
- `tools/check_presets.sh`: every tracked preset parses.
- `python3 tools/dap_smoke.py` against a running app: the generated presets and Build & Load still work.
- **Manual GUI check:**
  - The card picker lists all bundled cards.
  - An old saved instance without the version line is absent.
  - Name & Save writes a file with `format-version: 1`, and its autosave updates it.
  - Loading a preset with `slot-1-file:` + `slot-1-rom:` runs the ROM. Name & Save is unavailable for that slot.
- `tools/make_screenshots.sh 05-modules`: the real saves folder isn't touched.
- `pc1500_cli` with a `saveas: live slot-1:X` preset and `--save-dir` writes `X.card.yaml`, and the file loads again through `slot-1-file:`.
