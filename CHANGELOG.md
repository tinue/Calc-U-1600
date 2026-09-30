# Changelog

## [0.6.0] - work in progress

### New

- **Debugger** VS Code debugs machine code in the emulator: breakpoints
  (with conditions, hit counts, logpoints), data breakpoints, stepping in
  the `.asm` source or the disassembly, registers and memory, on the
  LH5801, the Z-80 and the LH5803, ROM code included. Build & Load
  rebuilds, reloads and restarts a program without detaching. Turn it on
  under **Settings ▸ Debugger** (or `--dap <port>`); the extension is in
  `vscode/calcu1600-debug/`. See docs/Debugger.md.
  - The VS Code extension works in any folder, without a `launch.json`:
    **Debug current file** on a PC-1600 (zasm) or PC-1500A (sdas), built-in
    build tasks, and settings for the port and assembler paths.
  - **Create Debug Project…** writes a starting point for a program or a
    ROM extension on either machine.
  - A preset's `debug:` block holds a project's debugger settings, and the
    launch configuration names it as `project`. `command` types a BASIC
    line to start the code.
  - **ROM extensions**: `bus-rom:` plugs ROM files into the expansion bus,
    Build & Load restarts with the rebuilt ROM, and `boot: debug` stops in
    the ROM's power-on code.
- **Drag and drop** Drop a preset, a BASIC program (`.bas` / `.bbin`) or
  machine code onto the window to load it; the content picks the loader,
  anything else is ignored. On macOS also onto the Dock icon, and presets
  open from Finder. See the User Guide, "Dropping files".
- **Keyboard** PC-1600: accented characters (`ä`, `é`, `ñ`, …) type
  through `KB II`, from host keys, Paste Text and a preset's `type:`, in
  the case typed. Dead keys compose (`¨` then `U` gives `Ü`).
- **Mount Directory** (File menu, PC-1600): a folder on the computer
  becomes drive `S3:` (also `Y:` without a CE-1600F). LOAD/SAVE, OPEN,
  FILES, KILL, NAME, COPY, SET and DSKF work on it. Only 8.3 names are
  shown, and INIT is refused. Subfolders work as on a MEP USB drive:
  `CDIR "path"` changes the current folder and `LDIR` lists subfolders.
  Programs written for the MEP module, such as FILEX, browse it. See
  docs/PC1600-Host-Drive.md and docs/PC1600-FILEX.md. The dialog opens in
  the directory last mounted, or in a fixed **Settings ▸ Host drive
  folder**.
- **Presets** New keys:
  - `host-drive: <folder>` mounts a folder as S3:.
  - `floppy-file: <file>` loads a `.floppy.yaml` next to the preset.
- **Presets** `saveas:` can write a file next to the preset
  (`saveas: template s2:file:Card.card.yaml`) and can save a template.
  Running the preset again replaces your own template of that name; a
  bundled name is refused.
- **`pc1600_cli --preset`** runs `saveas:` for cards too.
- **examples/dwx/** holds DiskWorks v3, self-contained: its card, floppy and
  S3: folder live next to the preset, and `make_diskworks_media.pc1600`
  replaces the two `setup/make_diskworks_*` presets.
- **Load BASIC Program** and a preset's `format: basic-binary` also take a
  tokenized program (`.bbin`, CE-158 or PC-1600 header). A PC-1500 one
  loads on the PC-1600 in MODE 1 only.
- **Debug panel** On the PC-1600, Pointers shows the program area `TITLE`
  selects, `MEM`, and each slot's program module with its free space.
- **Memory cards** A card definition's `encoding: file` block reads its
  binary next to the definition at every clean start, so a ROM module in a
  memory slot can be rebuilt.
- **Examples** Grouped by topic (`plotter/`, `basic/`, `interfaces/`,
  `machine-code/`, `memory/`, `dwx/`), with an index in
  `examples/README.md`.

### Changed

- **Loaders** On the PC-1600, Load BASIC Program, Load Machine Code and
  presets follow the calculator's MODE and `TITLE`, as `LOAD` does: in
  MODE 1 a listing is PC-1500 BASIC and a headerless file LH5801 code at
  an LH5803 address, and a program goes into the selected program area
  (S0, or an S1/S2 program module). The Load Machine Code dialog says
  which CPU and address space it assumes, and the advice after loading
  follows (`NEW &addr`, `XCALL`). An address in the work area
  (&F000-&FFFF) loads with a warning.
- **Presets** `program:` has no `slot:` any more; MODE and `TITLE` decide.
- **Debugger** The default port is 32168 (was 4711 in 0.6.0-pre). A port
  already saved in Settings is kept.
- **CE-1600P ROM** Switching it with the plotter attached swaps the box
  with an OFF/ON cycle instead of an ALL RESET: RAM and the inserted disk
  stay.
- **PC-1600 power** OFF really switches the machine off. ON, the WAKE$
  timer and an incoming call (CI) switch it on through the sub-CPU, as on
  the real unit; after an auto power-off, ON resumes where it stopped.
- **Presets** `saveas:` now needs `live` or `template` first:
  `saveas: live s2:My Card`.
- **Presets** `- modulespecfile:` is now `- modulespec-file:`, like
  `floppy-file:`.

- **Loaders** What a file is now comes from SharpDataExchange's library
  (0.3.1): Load Machine Code refuses a BASIC listing or tokenized BASIC
  (instead of loading it as raw bytes), skips `00` bytes before a header,
  and no longer checks the PC-1600 header's end marker. Building now
  requires the library on every platform.

- **Settings** Folder and serial-port paths show as `~/Calc-U-1600/…`
  (and `~/…` elsewhere under the home folder), as the User Guide writes
  them.
- **Plotter paper** Copy / Cut stay at 1200 DPI up to about 136 mm of
  CE-1600P paper and scale down beyond that (was ~339 mm), so a long plot
  no longer needs gigabytes of memory to copy.

### Fixed

- **Presets** On the PC-1500, a `program:` line that replaces a line of
  the same length (`10 A=1`, then `10 A=2`) is no longer reported as
  rejected.
- **Machine code** A header auto-run address of `&FFFF` (what the
  calculators write for "none") no longer proposes `CALL &FFFF`.
- **Presets** A machine-code header's auto-run into slot S2 now types
  `CALL #2,&<addr>`, as Load Machine Code proposes, instead of
  `CALL &<addr>`, which ran whatever bank 0 maps there.
- **Loaders** On the PC-1600, a BASIC program made of several program
  segments (a `#SEGMENT` or `99999` line in the listing) loads instead of
  failing with "the tokenized program is malformed".
- **Loaders** A fast-loaded BASIC program that crosses a memory module's
  16 KB bank boundary no longer splits a line across the two banks; it is
  laid out as the calculator's own `LOAD` does.
- **PC-1600 serial port** The TC8576F follows its data sheet: a CTS check
  (`RCVSTAT`/`SNDSTAT` 24) no longer blocks sending, the baud rate comes
  from the chip's registers, and the peer's RI reaches the calculator as
  CI.
- **Debug panel** Dump Card YAML also dumps an unbanked card (CE-1600M).
- **CLIs** `pc1500_cli` and `pc1600_cli` find the bundled memory cards
  (`Qt6/resources/cards`) without `--modules-dir`.
- **macOS** After the app had been killed, the "reopen windows?" prompt
  could hang the next start.

## [0.5.0] - 2026-09-25

### New

- **CE-158** RS-232C / Centronics interface, on the PC-1500/1500A and on
  the PC-1600 (MODE 1). Not yet together with the CE-1600P.
- **CE-1600P** New or old ROM, selectable.
- **Presets** `- saveas:` saves a memory card or floppy from the script.
- **Cards & floppies** `template: true` marks a template; the app never
  writes to one.
- **Keyboard** Scroll Lock / Insert -> `RSV`, more Shift variants, and a
  lone Shift tap latches the calculator's `SHIFT`.
- **Plotter paper** Long plots scroll; Copy / Cut export at 1200 DPI,
  without anti-aliasing, at physical size (scaled down past ~339 mm).
- **Docs** User Guide rewritten with screenshots; complete keyboard map;
  `.floppy.yaml` disk format spec.
- **Examples** `dampflok.bas`, a steam-locomotive sound demo.
- **Build** One root CMake project for app, CLIs and tests (debuggable
  in CLion, see `docs/Building.md`); scripted screenshots (`--shots`,
  `docs/screenshots/README.md`); `pc1600_cli --save-dir` for
  `saveas: floppy:`.

### Changed

- **Presets** The ROM is part of the model and plotter
  (`model: PC-1600:old`); `firmware:` is gone.
- **Paste** Paste Text types only the first line and doesn't press ENTER.
  After Load Machine Code, its `CALL` is typed in, ready for ENTER.
- **Presets** On the PC-1500, `format: basic-text` no longer presses `CL`
  and types `NEW0` itself -- add those steps to the preset, as on the
  PC-1600.
- **Control bar** Narrower ROM and floppy widgets.
- **Requirements** macOS 15.8 or later; SharpDataExchange 0.3.0.

### Fixed

- **Timing and sound** The PC-1600 now matches a real unit much more
  closely: BASIC speed, BEEP pitch and repeat rhythm, display and sub-CPU
  response times. The buzzer follows the real signal on both machines,
  shaped by a model of each one's piezo. BASIC on the PC-1600 still runs
  about 0.65% fast (see TODO.md).
- **PC-1600 interrupts and power** Interrupt handling follows the ROM and
  the Z-80 rules. A timer interrupt no longer runs in the middle of
  switching off, and an ON press during the handover between the two CPUs
  is no longer lost.
- **CPU cores** Chained Z-80 index prefixes, the R register, LH5801
  interrupt entry, and LH5803 I/O accesses to &8000-&BFFF.
- **PC-1600 serial port** An error reset clears the error flags; closing
  the serial peer no longer hangs a transmission.
- **BASIC and machine-code loading**
  - PC-1600: after a fast (`basic-binary`) load, LIST in PRO mode showed
    nothing until a MODE switch.
  - PC-1600: re-typing a line with the same length (`10 A=1` then
    `10 A=2`) failed the preset.
  - PC-1500: a `format: binary` block past &FFFF or into ROM was
    reported as loaded. Addresses like `41zz` are now errors.
  - A comment right after a block key (`memory-expansion-1:   # slot 1`)
    was a parse error.
  - `saveas:` failed on a slot loaded from a saved card, and on a second
    save of the same slot.
- **PC-1500 clock** `TIME=` followed at once by reading the time kept the
  old time.
- **App**
  - Crash on quit with TRACE on.
  - TRACE lost most instructions, and kept running empty after a machine
    rebuild.
  - A preset asking for a missing old PC-1600 ROM quit the app.
  - A failed plotter/interface attach (e.g. missing ROM) gave no message.
  - Re-picking the already-checked model/ROM menu item rebuilt the
    machine.
  - A saved card was rewritten every 500 ms while loaded.
  - Reset typed still-queued keys into the rebooting machine.

### Details

- **CE-158** Control-bar toggle, a printer pane for the Centronics
  output, and its own serial port (`calcu1600-ce158.serial`, see
  Settings). Presets attach it with `interface: ce158`.
- **CE-1600P ROM** Independent of the PC-1600 ROM; the CE-1600F follows
  it. Run `tools/fetch_roms.sh` again to get the old dump.
- **Preset ROM syntax** `model: PC-1500:A01`, `model: PC-1600:old`,
  `plotter: ce1600p:old`. A preset that still has `firmware:` fails with
  a hint. `model: PC-1500A` accepts only A04.
- **`saveas:`** `s1:<name>`, `s2:<name>` or `floppy:<name>` (the last
  two PC-1600 only); overwrites a same-named file. The DiskWorks setup
  presets use it.
- **Templates** No longer tied to shipping with the app: a template in
  the save folder (e.g. `examples/memory-cards/pc1500-maxed-out.card.yaml`)
  is listed with the bundled ones and offers Name & Save. Files without
  the key are instances, autosaved in place; existing ones need no change.
- **Keyboard** Shift+Delete -> Shift+`CL`, Shift+Home -> `CTRL`,
  Shift+End -> `KB II` (PC-1600), Shift+Page Up -> `DEF`. A Shift tap
  counts if released within 0.4 s with nothing in between. Full table in
  `docs/Keyboard-Mapping.md`.
- **Floppy format** SharpDataExchange (`sde dir/get/put/del`) reads and
  writes it. Eject a disk before editing its file with such a tool: the
  emulator rewrites the file after every disk access.

## [0.4.0] - 2026-09-20

- **Keyboard** Fixed a key getting stuck down and freezing the
  calculator (Reset didn't help). This happened when a host key that
  needs Shift (e.g. Shift-3 for `*` on a Swiss keyboard) was released
  after Shift. A key held while the window loses focus is now let go too,
  and Reset / Reset All release every key.
- **Keyboard** PC-1600: `_` is now typed as SHIFT + `.`, as on the real
  keyboard -- it used to come out as `9`.
- **ROM modules** Memory modules can now be ROM (`content: rom` in a
  card definition: read-only, bytes carried in the file). First one:
  Sharp's **CE-502B** Statistics module for the PC-1500/1500A. ROM
  modules have their own section at the bottom of the slot picker.
- **Presets** The built-in `- module: <name>` form (`ce155`, `ram16`,
  `ram32`, `ce1638plus`, `ce163f`) is gone -- name a module by its
  definition instead, e.g. `- modulespec: CE-155`. Every memory module
  now comes from a `.card.yaml` definition.
- **RAM** Powers up as zeros instead of &FF -- internal RAM and fresh
  memory modules, matching how CMOS RAM comes back after a power loss.
  Flash memory still starts erased (&FF). The exception is the PC-1500's
  &7600-&7BFF (display RAM and system RAM), which reads &FF after a power
  loss on the real machine and now does here too -- including the lock
  register at &79FF. On the PC-1500/1500A, Reset now
  keeps RAM (the BASIC program survives, like the real reset button) and
  Reset All clears it.
- **Settings** moved from the control bar to the menu bar: the
  application menu on macOS (Cmd-,), Edit > Settings… (Ctrl-,) elsewhere.
  The control bar's Reset button is gone too: Machine > Reset (Cmd-R)
  and Reset All (Cmd-Shift-R).
- **Keyboard** Keys held with Cmd/Ctrl (macOS) or Ctrl/Alt/Windows key
  (Windows/Linux) no longer type on the calculator -- e.g. Cmd-C no
  longer puts a "C" on the display. Option and AltGr still type.
- **Copy** With text selected in the debug panel's output, Edit > Copy
  (Cmd-C) copies that text instead of the screen.
- **Reset** Every reset runs the boot at full speed until the prompt
  appears, including a plotter's power-on init (the CE-1600P pen
  calibration), then sets the clock from the computer's time: Reset and
  Reset All, and also the reset that follows choosing a memory module, a
  ROM or a model. On the PC-1600 with a plotter the boot now really runs
  until the cursor appears; it used to stop early and finish in real time.
- **Paste** Edit > Paste Text (Cmd-V) types the clipboard's text into
  the machine, paced so no character is lost. Nothing is added or
  checked: a line break presses ENTER and waits for the line to finish,
  a trailing line break is dropped, and characters without a key are
  skipped. Pressing a machine key stops a paste in progress.
- **Copy** Edit > Copy Screen (Cmd-C) puts a PNG of the display on the
  clipboard: dot matrix only (no status indicators), black on white, at
  the real display's size (PC-1600 89 x 18 mm, PC-1500 104 x 5 mm).
- **Presets** New `- screenshot: <file.png>` step writes the same image
  into the trace directory. `trace:` and `screenshot:` now honour the
  trace directory set in Settings.
- **Presets** New `- syncclock:` step sets the calculator's clock to the
  host's current date and time. A preset load runs at full speed, which
  leaves the clock ahead; put `- syncclock:` last to correct it.
- **Presets** PC-1600 presets with a plotter (CE-1600P or CE-150) load
  much faster. The loader waited the full 15 s safety timeout before
  every `key:` and `type:` step (over 3 minutes for a short preset); it
  now continues as soon as the machine is ready for keys.
- **Settings** Separate start folders for the open dialogs: Samples
  (Load Preset), Basic (Load BASIC Program) and Assembly (Load Machine
  Code). Each defaults to `<last used>` -- the dialog
  opens in the folder a file was last loaded from. Choosing a folder
  fixes it; Reset returns to `<last used>`.
- **Machine code** File > Load Machine Code… loads a `.bin` into the
  running machine: with a CE-158 (PC-1500) or PC-1600 header, or raw.
  It asks only for the start address of a raw file. On the PC-1600 the
  code always goes into BASIC's program area (the one `NEW "S0:"`
  reserves in), and a raw file's address defaults to its start: &C0C5,
  or &80C5 when a RAM module is folded in as extension memory. Code
  meant for a program module (`INIT"Sx:","P"`, `NEW"Sx:",n`) needs a
  preset. Afterwards it shows the `NEW` that keeps BASIC from
  overwriting the code and the `CALL` that starts it. The code is never run automatically. A
  CE-158 file on a PC-1600 (LH5803 side) is not supported yet.
- **PC-1600** The calculator ROM version is selectable: the new ROM
  (default) or the old one (Machine > ROM Version, or `firmware: new|old`
  in a preset). If the old ROM can't be loaded, the app warns and uses
  the new one.
- **Plotters** Attaching or detaching a plotter now runs its OFF/ON
  power cycle at full speed (like Reset), including the power-on
  rotation of the pen header, and then sets the clock from the
  computer's time.
- **Control bar** Choosing a memory module or a ROM version no longer
  detaches the plotter or changes any other selector; the rebuild that
  applies it keeps the CE-150/CE-1600P you had (its paper starts blank).
  Switching the model starts from a default machine instead: PC-1500
  ROM A04 / PC-1600 new ROM, nothing attached, then the startup preset.
- **Debug** The pointer dump shows the PC-1500 lock register (&79FF) as
  locked or unlocked. The MODE key is ignored while it isn't &FF; `NEW0`
  unlocks it, so the startup presets now begin with `NEW0`.
- **Examples** Added CalCula, an RPN calculator program.
- **Build** `tools/fetch_roms.sh` fetches the PC-1600 new and old ROM
  sets and the CE-1600P ROMs from the updated PC-1600-ROM dumps.

## [0.3.0] - 2026-09-19

- **PC-1600F** Added the CE-1600F pocket floppy drive for the
  PC-1600. It attaches with the CE-1600P plotter/printer (no separate
  switch) and works like the real one: `INIT"X:"` formats a blank disk,
  and `SAVE`, `LOAD`, `FILES` and `DSKF` work on it. Disks are
  double-sided (64 KB per side); a control-bar button flips the disk to
  the other side, a green lamp shows when the drive motor runs, and
  drive timing follows the Service Manual.
- **Floppy disk** Two templates are available: A formatted floppy,
  and a blank one. Select one in the drop down to insert into the
  drive. Any changes you make on these templates are in-memory only.
  If you want to persist, click the "save" button and give the floppy
  a name.
- **Presets** Settings has a default preset per model (PC-1500,
  PC-1500A, PC-1600), applied at startup and whenever that model is
  selected. Ready-made ones are in `examples/startup/`; the PC-1600 one
  adds a CE-1600M and CE-1601M, mounts a floppy, sets up the RAM
  disk and COM1.
- **Loading** Long preset loads show a "Loading…" popup and the window
  keeps updating, instead of freezing.
- **Settings** The Settings dialog is reorganized into titled sections
  (General, Default presets, Storage, Tracing, Serial port).
- **Control bar** Common controls (Reset, Settings, model/ROM, memory
  slots) stay left-aligned, so switching models no longer shifts them.
  The redundant "Load Preset…" button is gone; use the File menu.
- **Build** Windows ARM64 is now built natively and gains BASIC preset
  loading; BASIC tokenizer updated to SharpDataExchange 0.2.1.

## [0.2.0] - 2026-09-18

- **Platforms** Added a native Windows ARM64 build, alongside the
  existing Windows x86_64, Linux, and macOS builds.
- **Keyboard** PC-1500/1500A live typing is now buffered, so typing
  quickly no longer drops keystrokes. Cursor keys (Left/Right/Up/Down)
  still pass straight through, so the ROM's own auto-repeat still works
  when held.
- **Startup** A missing or corrupted bundled ROM now shows a dialog
  naming the file and where it looked, instead of silently crashing.
- **Loading** Support loading segmented PC-1600 BASIC programs.
- **Loading** File ▸ Load Preset… no longer asks for confirmation before
  resetting the machine — picking a preset loads and resets immediately.
- **Loading** File ▸ Load BASIC Program… now behaves like a real LOAD:
  it no longer resets the machine, switches RUN/PRO mode, or types NEW0.
  It loads into whatever BASIC program area is currently live, so you can
  prepare the machine yourself first (fit a memory card, run `NEW`,
  attach a plotter) exactly as you would on real hardware.
- **Display** Press and hold anywhere on the LCD to fast-forward the
  emulation at full, unthrottled speed; release to return to normal
  speed.
- **Windows installer** Fixed a "missing MSVCP140.dll" failure on a
  clean machine by running the bundled Visual C++ redistributable
  during install.

## [0.1.0] - 2026-09-15
First public release. Calc-U-1600 is an emulator for the Sharp PC-1500,
PC-1500A, and PC-1600 pocket computers, built as a Qt6 desktop app for
macOS, Linux, and Windows.

- **Machines** All three models boot to BASIC, with keyboard, dot-matrix
  display, real-time clock, expansion-card, and serial-port support.
- **Plotters** CE-150 (PC-1500/1500A/1600) and CE-1600P (PC-1600) pen
  plotters, rendered on a virtual paper roll/sheet you can copy or export
  as an image.
- **Memory modules** Battery-backed and anonymous expansion-card modules
  per model, selectable and nameable from the control bar; state persists
  across sessions for named modules.
- **Presets** `.pc1500`/`.pc1500a`/`.pc1600` preset files describe a full
  machine configuration (model, ROM revision, modules, plotter, a resident
  program) and load it in one step via File ▸ Load Preset….
- **BASIC programs** File ▸ Load BASIC Program… tokenizes and loads a
  plain `.bas` listing directly into the running machine, without needing
  a preset wrapper.
- **Serial port** The PC-1600's RS-232C port is always live, backed by a
  host pseudo-terminal any serial application can connect to.
- **Native menu bar** File, Machine, and Help menus alongside the existing
  control bar, including an About dialog with version/build information.
