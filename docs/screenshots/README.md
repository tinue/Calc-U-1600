# Scripted screenshots

The pictures in [User-Guide.md](../User-Guide.md) (`docs/images/guide/`)
are generated, not hand-made. There is one scenario per guide chapter, in
[guide/](guide/). A **scenario** (`*.shots.yaml`) walks the real
app through a list of shots: each starts from a **preset**, which sets up
the emulator (model, cards, plotter, typed input). **Steps** then drive the
Qt UI, for example opening a drop-down, triggering a menu item or holding a
faceplate key, and a **capture** writes the PNG.

```sh
tools/make_screenshots.sh                    # every scenario in guide/
tools/make_screenshots.sh 05-modules         # just one (name without .shots.yaml)

# or directly:
Calc-U-1600.app/Contents/MacOS/Calc-U-1600 --shots docs/screenshots/guide/05-modules.shots.yaml \
    [--shots-out <dir>] [--shots-only name1,name2] [--shots-fail-fast]
```

The app shows its window, plays the scenario, prints one `[shots]` line per
shot or image to stderr, and quits. The exit code is:

- `0`: every image was written.
- `1`: at least one shot failed. Its line number and reason are printed,
  and the run carries on with the next shot unless `--shots-fail-fast` is
  given.
- `2`: the scenario itself doesn't parse.

That makes the same scenarios usable as a GUI smoke test.

## What a run guarantees

- **Frozen emulation.** Between steps the emulator doesn't run, so the LCD
  cursor, the clock and so on never drift. Only preset loads and the `key:`,
  `type:` and `run:` steps advance it, in emulated time. Qt-method images
  are byte-identical from run to run.
- **Isolated settings.** A run uses a throwaway settings file and an empty
  card/disk folder. Your default presets, last model, folders and saved
  cards never show up in the images, and the run never changes them. The
  Settings dialog shows the default folder (`~/Calc-U-1600`), as a fresh
  install does.
- **Qt's own file dialogs.** A run uses these instead of the native ones,
  so a script can capture them and close them again.
- **Fixed appearance and geometry.** A run uses light mode (see
  `appearance:`), the scenario's window size, and a fixed position on the
  primary screen.

## Scenario format

```yaml
out: ../images          # output folder, relative to this file (default: this folder)
window: 900x1000        # main-window size (default: leave as is)
appearance: light       # light (default) | dark | system
scale: 2                # device px per logical px for Qt captures (default 2)
settle: 150             # ms to let the UI settle after each UI step (default 150)

shots:
  - name: model-dropdown                       # unique; used by --shots-only
    window: 900x1000                           # optional: this shot's size (default: the scenario's)
    preset: ../presets/pc1500a-ready.pc1500a   # optional: load it first
    steps:                                     # optional
      - open: controlbar.model
    capture: { target: controlbar, file: model-dropdown.png, padding: 4 }
```

A shot without `preset:` continues from where the previous one left off.

The window is set to the shot's size when the shot starts, and again after
each `preset:` or `reset`, since a model switch changes the minimum size.
Two limits apply. A window can't be narrower than its layout: the PC-1600's
control bar needs about 1430. It also can't be taller than the screen. If
the real size differs from the requested one, the capture logs a
`note: window is …`. Give the shot a `window:` that fits: the plotter shot
uses `1430x1280`, so the paper panel shows the whole figure.
`capture:` on the shot is a final capture step. `capture: name.png` alone
captures the whole window. More captures can go in `steps:` as
`- capture: {...}`.

### Steps

| Step | What it does |
|---|---|
| `preset: <file>` | Load a preset, as File ▸ Load Preset… does. |
| `reset` / `reset: all` | Machine ▸ Reset / Reset All. |
| `key: <name>` | Tap a calculator key, as a click on the faceplate does. Key names are the preset `key:` names. |
| `hold-key: <name>` | Press a key and keep it down. The faceplate shows the press highlight. |
| `release` | Let go of the held key. |
| `type: <text>` | Type one line the way Edit ▸ Paste Text does, then press ENTER. |
| `run: <seconds>` | Run that much emulated time. |
| `settle: <ms>` | Wait wall-clock time, with the emulator still frozen. |
| `click: <objectName>` | Click a button. |
| `open: <objectName>` | Open a combo box's drop-down, or click a button. |
| `select: <objectName> = <item>` | Pick a combo-box item by its text. |
| `action: Menu > … > Item` | Trigger a menu item. `&` mnemonics are ignored, and `...` matches `…`. |
| `menu: Menu > Submenu` | Open a menu and leave it open. See the macOS note below. |
| `close` | Close the topmost popup, dialog, or native menu. |
| `choose-file: <path>` | Pick that file (relative to the scenario) in the open file dialog and accept it, e.g. after `action: File > Load Machine Code…`. |
| `enter-text: <text>` | Type into the text field of the open dialog, e.g. a name in Name & Save. |
| `capture: …` | Write an image; see below. |

A dialog opened by `action:` or `click:` stays open and scriptable: the
script carries on inside it, captures it, then `close`s it. Anything still
open when a shot ends is closed automatically, and a held key is released.

### Captures

`capture: { file, target, method, padding, scale }`

- `file` (required): a `.png` path relative to the output folder.
- `target`: one of these (default `window`):
  - `window`: the main window's contents.
  - `dialog`: the open dialog.
  - `plot`: the plotter's whole paper at physical size (up to 1200 DPI),
    as Copy puts it on the clipboard.
  - `lcd-image`: the LCD dot matrix at physical size, as Edit ▸ Copy Screen
    puts it on the clipboard. (`lcd` is the on-screen LCD widget.)
  - `screen-region`: `method: system` only; see below.
  - any **objectName** (listed below).
- `method`:
  - `qt` (default): the widget is rendered through Qt at `scale`, together
    with any open drop-down or menu, placed where it is on screen. This
    needs no permissions and doesn't depend on what else is on screen.
    There is no title bar, and native menu-bar menus can't be seen.
  - `system` (macOS): uses `screencapture`, so what's actually on screen,
    title bar and native menus included, at the screen's own resolution:
    - `target: window` or `dialog`: that window with its title bar, no
      shadow.
    - `target: screen-region`: while a `menu:` is open, the open menus plus
      the menu-bar strip above them (so the menu title shows); otherwise
      every window the app owns.
    - any other target: its on-screen rectangle.
- `padding`: logical px of margin. It's transparent with `qt`.
- `scale`: overrides the scenario's `scale` for this capture.

### Native menus (macOS)

With the macOS menu bar, `menu:` opens the real menu through System Events,
the way a user's click would. It can only be captured with
`method: system, target: screen-region`. The terminal, or whatever
launches the run, then needs these permissions under **System Settings ▸
Privacy & Security**:

- **Screen Recording**, for `screencapture`.
- **Accessibility** and **Automation ▸ System Events**, to open the menu.

A missing permission fails the shot with that message, never a silently
wrong picture. The guide uses `system` for only two shots: the title-bar
window in `01-introduction` and the Machine menu in `02-getting-started`.
Everything else is Qt-rendered and byte-identical from run to run.

Two macOS details:
- The app adds items of its own to any menu titled "Edit" (writing tools,
  dictation and so on, in the system language), so the guide doesn't show
  the Edit menu.
- The menu script brings the app to the front itself, because the menu bar
  belongs to the frontmost app.

### Widget names

| objectName | Widget |
|---|---|
| `faceplate`, `lcd` | calculator face / its LCD |
| `controlbar` | the whole control bar |
| `controlbar.model`, `controlbar.rom`, `controlbar.rom1600` | model / PC-1500 ROM / PC-1600 ROM pickers |
| `controlbar.slot1`, `controlbar.slot2`, `controlbar.slot1.save`, `controlbar.slot2.save` | module pickers and their Name & Save buttons |
| `controlbar.ce150`, `controlbar.ce158`, `controlbar.ce1600p`, `controlbar.ce1600p.rom` | peripheral toggles, CE-1600P ROM picker |
| `controlbar.floppy`, `controlbar.floppy.side`, `controlbar.floppy.save`, `controlbar.floppy.lamp` | CE-1600F disk row |
| `debugpanel`, `debugpanel.pointers`, `debugpanel.dumpmem`, `debugpanel.dumpcard`, `debugpanel.clear`, `debugpanel.trace` | Debug panel and its buttons |
| `paper`, `paper.copy`, `paper.cut` | plotter paper panel |
| `ce158printer`, `ce158printer.save`, `ce158printer.clear` | CE-158 printer panel |
| `dialog.settings`, `dialog.about`, `dialog.machinecode` | dialogs |
| `dialog.settings.general`, `.presets`, `.storage`, `.tracing`, `.serial` | one section of Settings |

## Presets

[presets/](presets/) holds the presets written for screenshots, so the
pictures don't change when an example preset does.

**Leave the calculator in RUN mode.** PRO mode is only right when the shot
shows a LIST or program entry. A capture taken in PRO mode logs a note. Start
from a clean `NEW0`, or the BASIC pointers (and so Debug ▸ Pointers, MEM
and so on) are garbage:

- PC-1500/1500A cold boot lands in PRO mode, so use `key: cl`, `type: NEW0`,
  then `key: mode`.
- The PC-1600 boots in RUN mode, and `NEW0` is only accepted in PRO mode,
  so use `key: mode`, `type: NEW0`, then `key: mode`. They are ordinary
presets (see [Preset files](../User-Guide.md#preset-files)) and may point at
files elsewhere in the repo, e.g. `../../../examples/plotter/lissajou-1600.bas`.
