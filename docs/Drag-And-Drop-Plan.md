# Drag-and-drop loading

**Status: done** (dev-0.6.0, 2026-09-30: da96dae, 324808a, 4c080b5, 3bd9ae7
and the docs commit). What changed against the plan below while implementing:

- **Headerless code also needs a `.bin` / `.rom` name** (3bd9ae7, user
  decision after testing). sde's heuristic alone took a JPEG, PDFs, two
  system fonts, `/bin/ls` and `dyld` for code. It rejected ZIP, gzip, PNG,
  random bytes and text. `dropfile::classify(bytes, fileName)` takes the
  name, and it only matters for that case.
- The class is `dropfile` in `Core/DropFile.{hpp,cpp}`. Its tests live in
  `program_file_tests.cpp`, next to the header builders they reuse.
- macOS file-open events go to a small `QApplication` subclass in
  `main.cpp`, not to an app-wide event filter on `MainWindow`. Such a filter
  sees every event of every object, and `MainWindow` already installs and
  removes one for the Shift tap.
- `dropEvent` hands the path on through a zero-time timer: a load and its
  dialogs inside the handler would keep Finder or Explorer waiting.
- The startup wait uses an `m_startupDone` flag as well as `busy()`. Opening
  a file at a cold launch can arrive before the startup preset has even
  started.

## Context

Today there are three ways to load a file, each behind its own File-menu dialog: Load Preset…,
Load BASIC Program… and Load Machine Code…. This change lets a file be dropped onto the emulator
window on macOS, Linux and Windows, and onto the Dock icon on macOS. The file's content decides
which loader runs, never its extension. Files that aren't recognized are silently ignored.

Decisions (user, 2026-09-30):
- Classify by content. For program files that means `sde_file_info` through `Core/ProgramFile`.
- A **preset** is recognized by sniffing for its mandatory top-level `model:` key. libsharpdx
  stays unchanged and presets get no magic line. sde can't do this job: a preset containing a
  character outside the Sharp set (such as `–`) doesn't come back as `text`.
- **Headerless** files are accepted only when sde's heuristic says they look like code
  (`raw-lh5801` / `raw-z80`), whatever the extension. Plain `raw` is ignored: data, JPEGs, files
  under 16 bytes. The guess only decides *whether* to accept a drop. MODE still picks the CPU,
  as the rule in Decisions.md requires.
- A drop runs the same loader as the matching menu item, on the running session.
- macOS: dropping onto the Dock icon (and Finder "Open With") is supported too.

## Phase 1: Core classifier (`Core/DropTarget.{hpp,cpp}` + tests)

- `Core/ProgramFile.{hpp,cpp}`: add `bool looksLikeCode` to `ProgramFile`. It is set for
  `raw-lh5801` / `raw-z80`, and the kind stays `Headerless`. A comment says it must never pick
  the CPU and is only used to decide whether to accept a drop. Update the `kindOf()` comment to
  match.
- New `dropfile::Target { None, Preset, BasicProgram, MachineCode }` and
  `Target classify(const std::vector<uint8_t>& bytes)`:
  1. **Preset first**: the bytes contain no NUL, are valid UTF-8 (an optional BOM is allowed),
     and one line starts at column 0 with `model:`. Commented or indented `model:` lines don't
     count. The comment points at `parsePresetFile()`'s `'model' is required`. The sniff goes in
     the new file, not in `PresetFile.cpp`, because that file has uncommitted user edits.
  2. Otherwise run `programfile::classify()`:
     - `BasicListing`, `BasicPC1500` and `BasicPC1600` → BasicProgram.
     - `CodeLH5801`, `CodeZ80`, and `Headerless && looksLikeCode` → MachineCode.
     - Everything else → None: Empty, Other (text, reserve, variables) and plain raw.
  - Damaged files and files whose length disagrees with their header still go to their loader,
    which shows its usual refusal: the file was recognized, it just can't load.
- The size cap goes in the GUI caller (see Phase 2), not here.
- Add both files to the Qt6 and CoreTests source lists in `CMakeLists.txt` / `Qt6/CMakeLists.txt`.
  The CLI build scripts don't need them.
- `Core/tests/drop_target_tests.cpp` (plus a `program_file_tests.cpp` case for `looksLikeCode`):
  - Presets: a preset → Preset; a preset containing `–` → Preset. YAML without `model:`, and
    with only `  model:` or `# model:` → None.
  - BASIC: `.bas` listing and `.bbin` (the existing `Core/tests/fixtures/basic/` lissajou files)
    → BasicProgram.
  - Machine code: a CE-158 or PC-1600 header → MachineCode. The `Z80_BLOCK`-style repeated code
    → MachineCode.
  - Ignored: random bytes, plain text, empty, a 10-byte blob → None.

## Phase 2: dropping onto the window (all platforms)

- `Qt6/app/MainWindow.{hpp,cpp}`: pull the menu lambdas out into path-taking methods, so the
  menu and the drop run the same code:
  - `loadPresetFile(path)`: `m_sync->loadPreset`.
  - `loadBasicProgramFile(path)`: `m_sync->run(... loadBasicProgramLive ...)`.
  - `loadMachineCodeFile(path)`: the second half of today's `loadMachineCode()`, from reading
    the file to the result box and the CALL paste, including `MachineCodeLoadDialog` when the
    address is needed.
  - Each one calls `AppSettings::rememberOpenFile` for its folder (Samples / Basic / Assembly).
    The menu handlers keep only the file dialog.
- `setAcceptDrops(true)`, plus:
  - `dragEnterEvent`/`dragMoveEvent`: accept only when all of these hold:
    - exactly one URL, and it's a local file;
    - `!m_sync->busy()`;
    - the file is ≤ 1 MiB, the cap that stops a huge file from being read on hover;
    - `dropfile::classify` ≠ None.
    Otherwise ignore the event, which gives the "not allowed" cursor and nothing else.
  - `dropEvent`: re-read the file, re-classify it, and dispatch through
    `openDroppedFile(path)`.
- `openDroppedFile(path)` is the one entry point for window drops and macOS file-open events.
  - It returns silently for None.
  - While `m_sync->busy()` it queues the path. A `SyncOperations::busyChanged(false)`
    connection drains the queue (last one wins).
  - It raises and activates the window.
- Child widgets that accept drops by default (the DebugPanel's text views) keep taking text
  drops. Only file-URL drops need to reach the window. Check that a file dropped on the
  faceplate, LCD and plotter paper reaches `MainWindow`.

## Phase 3: macOS Dock and Finder

- Qt delivers a Dock drop, "Open With" or double-click as a `QFileOpenEvent` to the
  `QApplication`. In the constructor, `MainWindow` installs itself as an event filter on
  `qApp`. `eventFilter()` handles `QEvent::FileOpen` → `openDroppedFile(event->file())`.
  - At a cold launch, the event can arrive while the startup preset (singleShot at
    `MainWindow.cpp:284`) is still running. The busy queue from Phase 2 handles that, so the
    dropped file loads after the startup preset.
- `Qt6/resources/macos/Info.plist.in`: a copy of CMake's default bundle template (keeping the
  `MACOSX_BUNDLE_*` substitutions) plus:
  - `UTExportedTypeDeclarations` for the presets (`ch.erzberger.calcu1600.preset`: `pc1500`,
    `pc1500a`, `pc1600`, conforming to `public.plain-text`).
  - `UTImportedTypeDeclarations` for `.bas` and `.bbin` (conforming to `public.data`).
  - `CFBundleDocumentTypes`, role Viewer: presets with `LSHandlerRank` Owner (so double-click
    opens them in Calc-U-1600). `.bas`, `.bbin` and `.bin` with Alternate, so they don't take
    over the default app for those files.
  - Point `MACOSX_BUNDLE_INFO_PLIST` at it in `Qt6/CMakeLists.txt`.
  - The Dock only highlights for these extensions. After that the content decides as usual,
    so a `.bin` that doesn't look like code is ignored.
- Windows and Linux: no icon or taskbar drop. They have no event for dropping on a running app,
  and a file argument at launch wasn't asked for.

## Phase 4: docs

- `docs/User-Guide.md`: a short "Drag and drop" part in the loading chapter. It covers which
  files are recognized, that anything else is ignored, the Dock on macOS, and that a drop loads
  into the running session the way the menu items do.
- `docs/Decisions.md` → "Loading programs": amend the `raw-lh5801`/`raw-z80` bullet. The guess
  is still never used for the CPU, but it does gate drops. Also add: presets are recognized by
  their `model:` key, not by extension or by sde, and unrecognized drops are ignored silently
  on purpose.
- `CHANGELOG.md` entry. Commit this plan as `docs/Drag-And-Drop-Plan.md`.

One commit per phase on `dev-0.6.0`. The user's uncommitted `PresetFile.*`, `preset_tests.cpp`
and `PresetDebugBlock.hpp` edits stay out of these commits.

## Verification

- CoreTests green after each phase (CMake build).
- App, via the `run` skill (`-ApplePersistenceIgnoreState YES`, quit via `calcu1600/quit`):
  - `open -a <built .app> examples/basic/hanoi.pc1600` loads the preset. This goes through the
    same `QFileOpenEvent` path as a Dock drop.
  - The same with `hanoi.bas` on a PC-1600 in PRO mode after NEW0, then `LIST`.
  - The same with a headered `.bin` from `examples/machine-code`: the result box appears and
    the CALL is pasted.
  - A cold launch through `open -a … file.pc1600` loads the file after the startup preset.
  - `open -a … some.png` does nothing.
- Manual by the user: drag each kind onto the window, including the LCD and the plotter paper.
  Also drag a JPEG, a text file, and two files at once to check the "not allowed" cursor, and
  drop onto the Dock icon. On Linux/Windows, a window drop on a CI build.
