> **Status (2026-09-30): implemented.** Where the work differed from the plan:
> - The four renames, the conversion and the parser change went into one
>   commit, so no commit has presets the parser refuses.
> - Headerless machine code in a preset needs no `.bin` / `.rom` name
>   (a drop does): the preset author named the file on purpose.
> - The parser now reads each `program: file:` to classify it; tests get
>   stand-in `x.bin` / `x.bas` files from `PresetTestSupport.hpp`.
> - Both CLIs got `--lcd-png`, used to compare every preset's final LCD
>   before and after. That found two presets that were already broken:
>   `lissajou-ce150-1600.bas` used `LLINE` in MODE 1 (fixed: `LINE`), and
>   `flashtest_ce163f.pc1500a` typed its lines in RUN mode (fixed), but its
>   program still stops with ERROR 1 IN 10 (TODO.md).
> - The DiskWorks templates were regenerated in a scratch copy; only their
>   time stamps differed, so the committed ones stay.

# Preset review: one inner logic for the format, and every preset up to date

## Context

The preset format (`.pc1500` / `.pc1500a` / `.pc1600`) grew one feature at a
time: `saveas:`, `floppy-file:`, `host-drive:`, `bus-rom:`, `debug:`, the
ROM suffix on `model:`. Each addition picked its own spelling. The 53 tracked
presets were written against different stages of the format, and at least
one no longer parses (`docs/developer/screenshots/presets/pc1600-modules.pc1600`,
TODO.md "Guide screenshots for chapter 5 fail").

Decisions already taken (AskUserQuestion):
- **Harmonize and break the old form.** No aliases and no "X is now Y" hints (pre-1.0 rule).
  The exception is `slot:`, which Decisions.md says to refuse with an explanation.
- **`type:` becomes fully literal.** Everything after `type: ` is typed, quotes included.
- **The extension matches the model.** Rename the files that don't match.
- **`format:` is inferred from the file** through `Core/ProgramFile` (`programfile::classify`).
  Only typing a program in has to be asked for.

## Findings

### A. Format: rules that disagree today (Core/Preset/PresetFile.cpp)
| Concern | Today | New rule |
|---|---|---|
| Product names | `model:` compared case-sensitively (`PC-1600`). `plotter:`/`interface:` are lower-case and accept `ce1600p` and `ce-1600p`. Modules are `CE-1600M` | Sharp's spelling with the hyphen (`PC-1600`, `CE-1600P`, `CE-150`, `CE-158`), matched case-insensitively. No hyphen-less alias. `none`/`off` dropped: leaving the key out means none |
| A file vs a name | `program: path:`, `bus-rom: - file:`, `modulespec-file:`, `floppy-file:`, `saveas … file:` | A path-valued key is `file` or ends in `-file`. `program: path:` becomes `program: file:`. `host-drive:` stays (it names a folder) |
| Memory slots | A one-item list: `memory-expansion:` / `memory-expansion-1:` / `-2:` holding `- modulespec[-file]:`. `saveas:` targets are `s1`/`s2`/`floppy` | Same shape as the floppy: `slot-1: CE-1600M` / `slot-1-file: my.card.yaml`, and `slot-2` likewise. The PC-1500 has `slot-1` only. `saveas:` targets are `slot-1`, `slot-2`, `floppy`, so a device word means the same thing everywhere |
| Numbers | `program: address:` is always hex (`4100` = &4100). `length:` is decimal unless `0x`. `bus-rom` values are decimal or `0x` | Everywhere: `&`, `0x` or `$` means hex, and a bare number is decimal. One helper, used by `program:` and `bus-rom:` |
| Bank qualifiers | `bus-rom:` takes `me1: true/false`. `debug:` listings take `me: 0/1` | `me: 0|1`, `pv: 0|1`, `pu: 0|1` in both |
| `wait:` | `std::stod` ignores trailing text, so `wait: 1s` passes | Strict: a number, or nothing |
| `type:` | Surrounding quotes are stripped. The workaround is `'"SAVE LOAD"'` | Literal to the end of the line. Comments go on their own line (as today) |
| `program:` | `format:` defaults to `binary`. `basic-binary` also takes `.bas` text. `basic-tokenized` is an alias | No `format:` key. `file:` is loaded by its kind (BASIC `.bas`/`.bbin`, machine code with or without a header; headerless code needs `address:` and a `.bin`/`.rom` name, as for drag and drop). `text: \|` is typed in. `file:` + `typed: true` types a listing in |
| `~/` | Expanded for `host-drive:` only. `saveas file:` gets a post-pass (`resolveSaveAsPaths`) | `resolvePath()` expands `~` for every path key. `parseStepList` gets the preset directory, and the post-pass goes (TODO.md item) |
| Dead parts | The `check` verb, `rom-modules:`, and the `firmware:` / `pre-load-keys` / `module:` hints | Removed. They become "unrecognized", except `slot:` (see Decisions.md) |
| `debug:` keys | camelCase (`stopOnEntry`, `cleanStart`). The rest of the file is kebab-case | **Stays camelCase on purpose.** They are the launch-configuration keys, and `stopOnEntry` is the DAP name. This is recorded in Decisions.md |

The header comment in `PresetFile.hpp` is also out of date: CE-150 on the PC-1600 as "Phase 2", `slot: S0|S1|S2`, "PC-1600 preset is model + memory slots + keys only", "`.pc1500` preset file", "no rom-modules/check". It gets rewritten to the new rules.

### B. Presets that are out of date (last change date in brackets)
- **Breaks:** `docs/developer/screenshots/presets/pc1600-modules.pc1600` has `saveas: s1:…` without `live`/`template`.
- **Extension ≠ model:**
  - These are `PC-1500A` saved as `.pc1500`: `examples/plotter/lissajou-1500.pc1500`, `dev/hardware-checks/adrtest.pc1500`, `dev/hardware-checks/lcdallon.pc1500`, `dev/presets/iterator.pc1500`.
  - Rename them to `.pc1500a` and fix `examples/README.md` and any other references.
- **`wait: 1s`:** the three `examples/setup/firmware_bootstrap_*.pc1500a`.
- **Fixed waits that mean "until done":** these switch to a bare `- wait:`.
  - `ce150-text-and-frame.pc1600` and `ce1600p-text-and-frame.pc1600` use `wait: 10`. Their trailing comment after the steps also goes.
  - `lissajou-ce150.pc1600` uses `wait: 600`.
- **No `NEW0` before a BASIC load** (the preset owns NEW0, Decisions.md): `examples/plotter/ascii.pc1600`, `examples/plotter/lissajou-1600.pc1600`.
- **Stale comments:**
  - `lcdallon` mentions `post-load-keys` and "plan.md Phase 2a".
  - `instrquirks` mentions `check: 0`.
  - `loader_test_1600_memory_cards` has the globus header copied in, plus the typos "RUM" and "paraneters".
  - `supercard_debug` says "with PC-1600P", but no plotter is attached.
  - `default-pc1600` has a header without the CE-1601M.
  - `default-pc1500(a)` says "go to RUN mode": check it.
  - `setcom1600` has a CTS "PSR-overlay guess". Check it against the current CPC model.
  - `debug-pc1500a` says "CE-163F (16K)".
  - `maxed-out-mem`: its README row says "program memory", but the preset uses `"M"`.
- **Style:**
  - Drop the default ROM suffix (`PC-1500A:A04` → `PC-1500A`; the same for `PC-1500:A04`).
  - Use 2-space indent everywhere (`maxed-out-mem` uses 1).
  - Write one comment line above each block, not after it.
  - `setcom1600` types `setcom1600.bas`. It becomes a normal fast load unless typing matters there; check that during verification.

## Implementation (phases, commit each to dev-0.6.0)

1. **Parser** (`Core/Preset/PresetFile.cpp/.hpp`, `PresetDebugBlock.hpp` for `me`):
   - Implement every rule in table A.
   - Add one number helper and one product-name matcher.
   - The `program:` kind comes from `programfile::classify` (`Core/ProgramFile.hpp`), reusing the check `Core/DropFile` already applies to headerless code.
   - Put the slot fields in `PresetFile` in the new form.
   - Adapt the consumers: `Core/PC1500/PC1500PresetLoader.cpp`, `Core/PC1600/PC1600PresetLoader.cpp`, `Core/Preset/PresetRunner.cpp` (switch on the classified kind, not on `Format`), `Qt6/app/PresetController.cpp`, `Qt6/app/debug/DapSession.cpp`, `Core/DropFile.hpp` and the CLIs in `tools/`.
   - Tests: update `Core/tests/preset_tests.cpp`, `pc1600_preset_tests.cpp`, `presetloader_trace_tests.cpp`, `ce150_tests.cpp`, `ce158_tests.cpp` and the inline presets in `PresetTestSupport.hpp`. Add cases for each new rule and each refused old form.
2. **Parse-only check:**
   - Add a `--check-preset <file>…` flag to `tools/pc1600_cli.cpp` (parsePresetFile is family-agnostic). It parses and reports, and boots nothing.
   - Add `tools/check_presets.sh`, which runs it over `git ls-files '*.pc1500' '*.pc1500a' '*.pc1600'`. Tests don't read `examples/` (Decisions.md), so this stays a tool.
3. **Presets:**
   - Rewrite all 53 to the new form and fix everything in list B.
   - The VS Code templates and default presets (`vscode/calcu1600-debug/...`) are included; users' existing debug presets break, which is accepted pre-1.0.
   - `make_diskworks_media.pc1600` gets re-run, so its templates `CE-1601M - Progs.card.yaml` / `Progs.floppy.yaml` are regenerated. Diff them before committing.
4. **Docs:**
   - `docs/User-Guide.md` ch. 8: all examples, the 8.7 table and the 8.9 reference.
   - Other docs: `docs/Debugger.md` (the `bus-rom:` `me`, `program: file:`), `examples/README.md` and `docs/background/Decisions.md`. The new Decisions entries are: preset = YAML-shaped, not YAML; `type:` is literal; `debug:` stays camelCase; the naming rule; extension = model.
   - Also update `CHANGELOG.md`. In `TODO.md`, remove the chapter-5 screenshot item and the `~/` item.
   - Save this plan as `docs/background/plans/Preset-Review-Plan.md`, with a row in `docs/background/README.md`.

## Verification
- Build and run CoreTests with the new parser cases. Everything must pass.
- `tools/check_presets.sh`: all 53 presets parse. Also run the old versions of a few presets through it to see that each old form is refused with a clear message.
- Run each non-GUI preset headless (`build/pc1500_cli --preset …`, `build/pc1600_cli --preset …`) and compare the LCD/BASIC dump with a run before the change.
  - This covers the NEW0 additions, the bare waits and `setcom1600`.
  - Run `make_diskworks_media` in a copy under the scratchpad first.
- Run the scripted guide screenshots for chapters 5 and 8 (`--shots`, docs/developer/screenshots/guide). The chapter-5 pickers must be written again.
- App: File ▸ Load Preset with DiskWorks.pc1600 and default-pc1600.pc1600. VS Code: a new project from each of the 4 templates, then F5. That last check is the user's, as before.
