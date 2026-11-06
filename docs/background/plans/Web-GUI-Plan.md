# Web GUI: a WebAssembly frontend on the Shared layer

> **Status (2026-11-06): implemented** (d3e394f … this commit); developer docs in docs/developer/Web-Client.md. Differences from this plan:
> - **Phase 0.2:** drop dispatch stays per frontend (Core's `dropfile::classify` decides; holding a drop while busy is event timing). The machine-code flow became one class, `Shared/MachineCodeLoad` (plan, address check, request, loaded advice), with `programChoice()` beside it.
> - **Phase 0.4 dropped:** IDBFS's `autoPersist` mount option writes every change to IndexedDB itself (Core's tape and preset writes too), so Shared needs no `filesChanged()` hook.
> - **Added in Phase 0:** `AppSession::pickModelJob()` / `defaultPresetJob()` (model pick + its default preset) and `Shared/CardYamlDump` (the INSPECTOR's Card YAML), both moved out of the Qt app.
> - **Phase 1:** `Web/build.sh` drives CMake (target `calcu_web`, in `build-wasm/`) instead of calling `em++` directly; the curated presets exclude the CE-158 demos (they wait for serial input).
> - **Phase 3:** hit-testing and the LCD drawing run in the page (same Shared numbers, no worker round trip). A host key's release is held back to 50 ms after its press: both events could land in one frame and the press was never scanned.
> - **Phase 5:** `wasm-check` became the `web` job. A minimal developer doc (Web-Client.md) was written since the embed contract needs one; user docs, the parity table and the calc-u.org page stay in TODO.md.


## Context

Core/Shared/Qt6 layering is done (Shared-Layer-Plan, Followup-Plan); TODO.md
"Shared layer and web GUI: still open" lists the web GUI as the next frontend.
Goal: a browser client that looks like the Qt6 app, built only on `Shared/`,
in two forms from one `dist/`:
- an **embeddable component** for a web page (Calc-U-59 `WebClient.md` contract);
- an **installable offline PWA** for tablets (landscape; portrait just stacks;
  phones are not designed for).

Settled with the user:

| Topic | Decision |
|---|---|
| ROMs | Bundled like the desktop release (`roms/` copied into `dist/`, precached) |
| Persistence | Saved cards, disks, tapes and settings live in IndexedDB across visits, with import/export as files. The embedded client and the PWA share storage (same origin); one tab writes, other tabs warn |
| Inspector | Included (Pointers, Memory, Dump, Card YAML, Clear); no TRACE |
| Host drive S3: | Left out |
| Serial (RS-232C, CE-158 serial) | Left out. The CE-158 printer panel still works |
| DAP debugger | Left out (not in Shared) |
| Menus | In-page File / Edit / Machine / Help menu bar above the faceplate |
| Layout | Faceplate scaled to fit the width. Control bar, then the panels (paper, printer, inspector) in the space below |
| Tapes | Tape picker and Save as on desktop; tapes kept in storage; WAV import/export |
| Presets | Curated `presets.json` subset plus "Upload preset…". The default preset per model is picked from that list |
| Embedding | Vendored-React component plus `dist/` zip, same as Calc-U-59 |
| In scope | Shared prerequisites, the web client, CI job and release zip |
| Out of scope | calc-u.org/1600 play page, user docs, parity table |

## Phase 0: Shared prerequisites (the Qt app switches over too)

Per CLAUDE.md no decision goes in a frontend, so these move out of `Qt6/app/MainWindow.cpp`:

1. **UI state from Shared.** Add `AppSession::controlState()`, returning a
   struct: per `Peripheral` `{offered, attached, enabled}`, slot-2 visible,
   floppy row visible/enabled, tape row visible, which ROM menus are shown,
   host-drive actions. It replaces the rules in
   `MainWindow::syncControlBarForModel()` / `syncPeripherals()`
   (`MainWindow.cpp:313,533`), which then just render the struct.
   Frontend capabilities (no host drive, no serial on the web) come from a
   `Platform` capability flag, so the web never offers them.
2. **File-open flows as two-step APIs.** New `Shared/FileOpen.hpp/.cpp`:
   - `programImages(bytes)` → one image, or a list of names for a
     multi-file WAV, so the frontend asks "Load which one?" (from
     `readProgramFile`, :412).
   - `machineCodePlan(bytes)` → `machinecode::Plan` plus the PC-1600 load
     state (from `loadMachineCodeFile`, :448–484).
   - After the load, `machineCodeLoaded(request)` → a struct with the
     summary line, NEW/CALL commands and notes (the RAMST/RAMEND peeks and
     `machinecode::advice` move here, :488–516). The frontend shows it,
     then `pasteText(callCommand)`.
   - The live address hints in `MachineCodeLoadDialog` move to a Shared
     function too, if any logic is there.
   - Drop dispatch: `AppSession::openFile(name, bytes)` classifies with
     `dropfile::` and holds a drop that arrives while busy (from
     `openDroppedFile`, :861–880).
3. **Media-list cache** in `ModuleBay` / `FloppyBay` / `TapeBay`
   (TODO.md item). It is invalidated on save-as, name-and-save, delete and
   import, so a resync doesn't re-parse full-RAM `.card.yaml` files.
   This matters more on the web.
4. **Persistence hook.** `Platform::filesChanged()` (optional) fires after
   `atomicWriteFile` (`Shared/AppPaths.cpp`), card/disk autosave and tape
   saves. The web uses it to trigger a debounced IDBFS `syncfs`.
5. **Media management** for the web (also usable by Qt later): list, delete,
   export bytes and import bytes into the instance/tape dirs, on the bays.

Each sub-step: `tools/run_tests.sh` green, plus Shared unit tests in
`Shared/tests/`, `tools/build_wasm_check.sh` green, the Qt app builds and is
spot-checked (Load Machine Code dialog, multi-file WAV, drop, peripheral
toggles). Commit per step on `dev-0.8.0`.

## Phase 1: Web build skeleton (`Web/`, modelled on Calc-U-59 `Web/`)

- `Web/bindings.cpp` holds only type translation: a `WebController` that owns
  `SettingsStore`, `Settings`, `AppPaths`, `AppSession`, a `Platform` impl
  and an `AudioSink`. Embind class with methods for tick, keys,
  pointer/face keys, jobs, file opens, media, inspector and clipboard
  sources.
- CMake target `calcu_web` under `EMSCRIPTEN` (next to `wasm_smoke` in
  `CMakeLists.txt:55–73`). Flags: `-fwasm-exceptions` (required by
  libsharpdx-wasm), `-lembind`, `-lidbfs.js`, `MODULARIZE`,
  `ENVIRONMENT=web,worker`, `ALLOW_MEMORY_GROWTH`. No `-pthread`, because
  static hosts can't be relied on for COOP/COEP.
- `Web/build.sh` runs `emcmake` + `cmake --build`, then copies:
  - `roms/` and `Qt6/resources/cards/*.yaml` into `dist/bundle/`;
  - the faceplate PNGs into `dist/img/`;
  - the curated presets into `dist/presets/`.
  Then `Web/build.js` (vendored Babel, no npm runtime deps, as Calc-U-59)
  produces `calc.js`, fills the service-worker asset list plus a content-hash
  cache version, and writes `MANIFEST.json`.
- `Web/vendor/`: React 18, ReactDOM, Babel (build only), ReactDOMServer
  (tests). Copied from Calc-U-59.
- `Web/dist/` goes in .gitignore.

## Phase 2: Engine worker

`Web/calc-engine-worker.js` runs one `WebController`, ticking
`setInterval(1000/60)`. It is paused on `visibilitychange` (the Pacer drops
the stall).

- **File system:**
  - `/bundle` is MEMFS, filled at init from fetched files (ROMs ~344 KB plus
    the card templates; everything up front, no lazy XHR needed).
  - `/home` is IDBFS (the instance cards, floppies and tapes dirs that
    `AppPaths` derives).
  - `syncfs(true)` runs before `start()`. `syncfs(false)` is debounced on
    `filesChanged()` and runs on `pagehide`.
- **Single writer:** `navigator.locks` "calcu1600-storage". A tab that
  doesn't get the lock runs without persisting and shows a banner.
- **Settings:** `SettingsStore` is backed by a map seeded from `localStorage`
  (`calcu1600/` prefix). Changes are posted back as `store` messages, as
  Calc-U-59 does.
- **Messages:** generic `call {method,args,id}` RPC plus pushes:
  - `display`: packed `DisplayFrame` bits plus indicators, only on change;
  - `controlState`;
  - `plot`: new strokes by plot revision;
  - `printer`: CE-158 text;
  - `busy` / `result`;
  - `notice`, `choose` prompts;
  - `audio` chunks;
  - `store`.
- **Audio:** the `AudioSink` posts int16 chunks to the page. An
  AudioWorklet ring buffer plays them, and `room()` is estimated from
  written samples minus elapsed time. The AudioContext resumes on the first
  user gesture.

## Phase 3: UI, visually equivalent to Qt6 (`Web/src/*.jsx`, `calc.css`)

- **`MenuBar`**:
  - **File:** Load Preset ▸ curated list / Upload…, Load BASIC Program…,
    Load Machine Code…, Import… (card, disk, tape, preset), Manage Saved
    Media… (list, download, delete).
  - **Edit:** Copy Screen (image + text through `navigator.clipboard`),
    Paste Text, Settings….
  - **Machine:** Model, ROM Revision, ROM Version, CE-1600P ROM,
    Peripherals, Reset, Reset All.
  - **Help:** About.
  - Items are shown and enabled from `controlState`.
- **`Faceplate`**:
  - a canvas with the PNG, fitted with Shared `fitLayout()`;
  - the pressed key gets a white 25 % overlay;
  - pointer capture, with hit-test through Shared `hitTest()`;
  - one key per pointer, so SHIFT and a key can be held on a tablet;
  - holding the LCD runs turbo;
  - transparent ARIA buttons over the keys.
- **`Lcd`** canvas: the `LcdWidget.cpp` algorithm (0.9-pitch dots, colours
  and `indicators()` from `Shared/LcdLayout.hpp`).
- **`ControlBar`**: same widgets and order as `Qt6/app/ControlBar.cpp`
  (model, slots plus save, peripheral toggles, Disk/side/lamp, Tape/Save
  /counter).
- **Panels** below, styled like `ChromeColors.hpp` (header strip, pill
  buttons, light/dark via `prefers-color-scheme`):
  - `PlotterPaper`: strokes from `buildPaperStrokes()`, auto-scroll,
    Copy/Cut as a PNG at `paperExportSize()`, with download as a fallback.
  - `Ce158Printer`: text log; Save downloads the raw bytes.
  - `Inspector`: Pointers, Memory ▾, Dump ▾, Card YAML, Clear; no
    TRACE/LOG. Shown from roughly 1000 px width.
- **Dialogs**: Settings (startup model, default preset per model), About,
  Machine Code Load (address, slot and hints from Shared), "Load which
  one?", Name & Save, Save Tape (with a replace prompt), warnings, and a
  Loading… popup after 500 ms.
- **Host keyboard**: `KeyboardEvent.code` → `shared::HostKey` →
  `resolveHostKey` (with the mac flag from the platform). On top of that:
  - auto-repeat ignored;
  - a Shift tap of 400 ms or less taps SHIFT;
  - composition events give one tap per character;
  - all keys released on blur;
  - keys with ⌘/Ctrl are never forwarded.
  This matches `MainWindow`'s handling.
- **Drag and drop**: files up to 1 MB go to `openFile`.

## Phase 4: Embed and PWA

- **Embed:** the `calc.js` global `CalcU1600` component, with props `width`,
  `height`, `keyboard`, `model`, `preset`, `workerUrl`.
  - URLs resolve from `document.currentScript`, so it works from any
    location.
  - It renders without side effects (worker, fetch and listeners start only
    in effects), so it can be prerendered.
  - CSS is scoped to `.calcu-client`.
  - Stable `data-*` test hooks.
- **PWA (`Web/app/`):**
  - `index.html` with the Apple meta tags;
  - `manifest.json` (standalone, icon from `Qt6/resources`);
  - `sw.js`: cache-first precache of the shell, ROMs, cards, presets and
    faceplates, with the cache named per scope plus a hash;
  - an `engine-worker.js` shim inside the SW scope, so the worker loads
    offline;
  - an install banner (`beforeinstallprompt`, or an iPad hint).

## Phase 5: Tests, CI, release

- **Playwright** (`Web/package.json` dev dependency only; `Web/run-tests.sh`
  serves `dist/` at the root and under a subpath). Projects: root, subpath,
  offline-chromium, offline-webkit. Check the LCD through `lcdText` (text
  before images):
  - boot all three models;
  - `PRINT 6*7` gives `42`;
  - a faceplate press/release;
  - a host key;
  - model switch and peripheral toggles follow `controlState`;
  - a plotter preset produces strokes;
  - Name & Save a RAM card, reload the page, the card is still there;
  - tape record, export and re-import;
  - the second-tab banner;
  - a multi-file WAV prompt;
  - the machine-code dialog and advice;
  - offline start of the PWA;
  - prerender in Node.
- **CI:** a `web` job in `.github/workflows/` (emsdk 6.0.13, node 24): build,
  then Playwright, then the `Calc-U-1600-web-<ver>.zip` artifact.
  `release.yaml` attaches `Calc-U-1600-web.zip`.
- **Docs, kept minimal:** commit this plan as
  `docs/background/plans/Web-GUI-Plan.md` plus a row in
  `docs/background/README.md`. Add `docs/background/Decisions.md` entries
  for: no pthreads, IDBFS rather than OPFS (WasmFS OPFS needs threads),
  ROMs bundled, single writer through Web Locks, and S3:/serial/DAP left out
  on the web. Update the TODO.md section.

## Verification

- After each Phase-0 step: `tools/run_tests.sh`,
  `tools/build_wasm_check.sh`, the Qt app builds and is spot-checked.
- Web: `Web/build.sh && Web/run-tests.sh` (all Playwright projects green).
  Then a manual check in Chrome (via claude-in-chrome) and Safari:
  - layout against the `docs/images/guide/` screenshots (overview, control
    bar, plotter paper, CE-158, inspector, dialogs);
  - audio BEEP;
  - Copy Screen and paper copy into an image editor;
  - PWA install and offline start.
- Install on an iPad (served over HTTPS) to check touch, two-finger SHIFT
  plus key, and persistence after the app is closed.
