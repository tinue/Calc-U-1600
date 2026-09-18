# Calc-U-1600 User Guide

Calc-U-1600 emulates three Sharp pocket computers — the **PC-1500**, the
**PC-1500A**, and the **PC-1600** — as a single desktop app. This guide
covers using the emulator itself: the control bar, the host-keyboard
mapping, memory modules and the plotter, saving battery-backed module
state, the Settings panel, and (for anyone going further) the Debug
panel, authoring custom memory-card definitions, and preset files.

This is a first-draft, plain-Markdown guide. A future version will grow
into a full browsable site (the way the sibling Calc-U-59 project's docs
site works); the section breakdown below is written to map cleanly onto
that later.

- [Getting started](#getting-started)
- [The calculator & keyboard](#the-calculator--keyboard)
- [Memory modules & plotter](#memory-modules--plotter)
- [Saving your work (battery-backed modules)](#saving-your-work-battery-backed-modules)
- [Settings](#settings)
- [Advanced topics](#advanced-topics)
  - [Debug panel](#debug-panel)
  - [Custom YAML memory cards](#custom-yaml-memory-cards)
  - [Preloaded-data cards & Dump Card YAML](#preloaded-data-cards--dump-card-yaml)
  - [Preset files](#preset-files)

## Getting started

Launch the app and it boots straight to BASIC on a PC-1500A (the default
model) — no separate "power on" step. The control bar above the
calculator's own faceplate has, left to right:

- **Reset** — press the calculator's own reset. A plain click is a
  normal reset (memory and BASIC program untouched, same as the real
  hardware's reset button). Cmd-click performs an "ALL RESET" — on the
  PC-1600 this is the deeper reset level that also re-seeds the clock;
  the PC-1500/1500A have no distinct ALL RESET, so Cmd-click there is the
  same as a plain click.
- **Slot 1 / Slot 2** — the memory module pickers; see
  [Memory modules & plotter](#memory-modules--plotter) below. Slot 2 only
  appears for the PC-1600, which has two expansion slots.
- **Model** — switch between PC-1500, PC-1500A, and PC-1600. Switching
  rebuilds the machine from scratch (a cold restart of the emulation, not
  a real device's model change).
- **ROM revision** — PC-1500 only (the PC-1500A always runs its one ROM,
  and the PC-1600 has no revision choice at all): A01, A03, or A04.
- **Open Preset…** — load a `.pc1500`/`.pc1500a`/`.pc1600` scenario file;
  see [Preset files](#preset-files).
- **Settings…** — see [Settings](#settings).
- **CE-150 / CE-1600P** — plotter attach/detach toggles; see
  [Memory modules & plotter](#memory-modules--plotter).

## The calculator & keyboard

The host keyboard drives the calculator directly — there's no separate
"click the on-screen keys" requirement, and the mapping is fixed (not
user-remappable). The practical mapping:

- **Letters, digits, and `+ - = * / ( ) .`** type directly.
- **Space**, **Enter/Return** (→ `ENTER`), and the **arrow keys** work as
  expected.
- **Backspace** → the calculator's own backspace-equivalent key (`bs` on
  the PC-1600, `left` on the PC-1500/1500A — matching where the real
  keyboards put it).
- **F1–F6** → the calculator's six function keys.
- **Tab** → `MODE` (RUN/PRO toggle).
- **Delete** → `CL`.
- Any host key that types a **shifted punctuation character** already
  printed as the calculator keyboard's "second legend" (e.g. `!`, `#`,
  `@`) is mapped straight to that character's base key with Shift held —
  you don't need to hold the calculator's own Shift separately, typing
  the character on your host keyboard is enough. On the PC-1600, this
  also covers the numeric keypad's own second-legend row.
- A few navigation-cluster keys, reachable only from an external/PC-style
  keyboard, map to calculator keys that don't have an obvious host
  equivalent otherwise — these are this project's own choice, not a
  documented hardware correspondence: **Scroll Lock** → `RSV`, **Home** →
  `RCL`, **End** → `SML`, **Page Up** → Shift+Left, **Page Down** →
  Shift+Right.

The on-screen faceplate shows the calculator's actual physical key
layout (which differs between the PC-1500 and PC-1600) for reference —
you don't need to click it, but it's there to look up a key's real
position/label.

## Memory modules & plotter

Each module slot has its own combo box in the control bar. Opening it
lists, in order: **–empty–**, then the bundled standard modules
compatible with the current model/slot, then (below a separator, if any
exist) your own previously-saved battery-backed instances. Picking an
entry attaches that module immediately (this rebuilds the machine, the
same as a model switch).

The PC-1600 has two independent slots; the PC-1500/1500A have one. A
module that's battery-backed shows a small save-icon button next to its
slot's combo — see the next section.

The **CE-150** and **CE-1600P** buttons attach/detach the corresponding
pen plotter. The CE-1600P button only appears for the PC-1600 (the
CE-150 button stays available there too, since the PC-1600 can drive
either plotter); whichever one is currently attached disables the other
so only one plotter is ever live at a time. Matching real hardware, where
hot-plugging a peripheral needs a power cycle for the ROM to notice it,
clicking one runs a synthesized OFF/ON power cycle rather than attaching
instantly.

## Saving your work (battery-backed modules)

Some memory modules are battery-backed on real hardware, meaning their
contents survive being unplugged. Calc-U-1600 models this: for a
battery-backed module, the slot's save-icon button (tooltip "Name &
Save") is visible.

Click it, type a name, and the module's current contents are written out
as a standalone `<name>.card.yaml` file. From then on, that slot
**autosaves**: any write to the module is written back to that same file
after a short (500 ms) debounce, so you don't need to click Save again —
just don't switch models/slots away before the debounce window if you
just made a change and immediately want to be sure it landed (in
practice, half a second is short enough this is rarely a concern).

Once saved, the instance shows up in that slot's combo box (below the
separator, alongside any other saved instances) so you can reattach it
later, or reference it by name from a preset file.

Where these files land is controlled by **Settings ▸ Battery-card save
directory** (default `~/Calc-U-1600`).

## Settings

Opens from the **Settings…** button. Every control here writes through
immediately — there's no separate Save/Cancel, just **Close**.

- **Startup device** — which model to boot into next time the app
  launches: Last used, PC-1500 (ROM A04), PC-1500A, or PC-1600.
- **Battery-card save directory** — where "Name & Save" instances and
  autosaves land (see above). Change or reset to the default.
- **Default "Open Preset…" folder** — the folder the preset file-picker
  opens on. Change or reset to the system default.
- **PC-1500 / PC-1500A / PC-1600 default preset** — a preset file
  applied whenever that model gets selected (on a model switch, and at
  startup). A model switch never carries modules over from the previous
  model, so this is how a model gets its standard setup — e.g.
  `examples/default-pc1500.pc1500` (CE-150 + CE-163F) or
  `examples/default-pc1600.pc1600` (CE-1600P + CE-1600M in Slot 1).
  Reset to go back to a bare machine.
- **Trace file save directory** and **maximum trace file size** — where
  CPU instruction traces are written (see the Debug panel's TRACE
  button below) and a size cap (in MB) before a running trace stops
  itself.
- **Serial Port** (PC-1600 only; not shown on Windows) — shows the
  live path of the `calcu1600.serial` symlink a host serial client
  should connect to, and lets you change the directory that symlink
  lives in.

## Advanced topics

### Debug panel

A dockable panel under the control bar with a scrolling log and a row of
buttons:

- **Pointers** — dumps the BASIC/system pointer table (program-end,
  RAM-end, and similar work-area pointers) for the current model.
- **Dump Mem** — dumps labeled memory regions (module, built-in RAM,
  display RAM, system RAM, etc.), skipping runs of `00`/`FF`/`AA` filler
  so the interesting bytes aren't buried.
- **Dump Card YAML** — captures the currently-attached module's contents
  as an `initial-content:` block, ready to paste into a `.card.yaml`
  definition. See [Preloaded-data cards & Dump Card
  YAML](#preloaded-data-cards--dump-card-yaml) below.
- **Clear** (trash icon) — clears the log.
- **TRACE** — starts/stops a CPU instruction trace to a file (under the
  directory set in Settings, capped at the configured size).
- **LOG** — a placeholder toggle; there's nothing behind it yet.

### Custom YAML memory cards

Every memory module — real Sharp hardware or not — is described by a
`.card.yaml` file: which connector pins enable it, what kind of storage
sits behind them, and (optionally) what bytes it starts with. This
section is a practical, example-first walkthrough; the full field-by-field
reference lives in
[`docs/Memory-Card-Definition-Spec.md`](Memory-Card-Definition-Spec.md)
(the *what*/*why*) and
[`docs/Memory-Card-Definition-Format.md`](Memory-Card-Definition-Format.md)
(the *how* — every YAML key, spelled out).

The sample for this walkthrough,
[`examples/memory-cards/pc1500-maxed-out.card.yaml`](../examples/memory-cards/pc1500-maxed-out.card.yaml),
describes a card that **never existed in reality**: it wires every
general-purpose expansion pin the PC-1500 mainboard has — `Y0`'s full
16 KB window plus the three bare 2 KB `S1`/`S2`/`S3` strobes — to one flat
22 KB SRAM range, for both the PC-1500 and PC-1500A:

```yaml
module-name: "PC-1500 Maxed-Out (fictional)"
compatible-hosts: [PC-1500, PC-1500A]
definition-terminology: PC-1500

regions:
  - name: sram-22k
    capacity: 0x5800   # 16 KB (Y0) + 3 x 2 KB (S1/S2/S3) = 22 KB
    banking: none
    content:
      kind: regular
      writable: true
      power-up-fill: 0xFF
    addressing:
      any-of:
        - chip-select: Y0
          span: 0x4000
          maps-to: 0x0000
        - chip-select: S1
          span: 0x0800
          maps-to: 0x4000
        - chip-select: S2
          span: 0x0800
          maps-to: 0x4800
        - chip-select: S3
          span: 0x0800
          maps-to: 0x5000
```

A few things worth noticing, reading top to bottom:

- `module-name` is what shows up in the control bar's module picker, and
  what a preset's `modulespec:` resolves against.
- `compatible-hosts` is the loader's *only* compatibility check — list
  every host this file is approved for; a host not on the list is
  declined outright.
- `definition-terminology` fixes which pin-name vocabulary the rest of
  the file uses (`Y0`/`S1`-`S4` for `PC-1500`; different names for the
  PC-1500A/PC-1600 slots — see the format doc's terminology table).
- Under `addressing`, `any-of` is an OR of independent groups — each
  `chip-select` here is a bare mainboard strobe with no address decoding
  of its own, so each just claims a `span` of the backing store at a
  `maps-to` offset. (A region that also gates on specific address lines,
  like the real CE-155's `Y0` chip, would add an `address-bits` term —
  see `ce155.card.yaml` for that shape.)
- `capacity` must equal the sum of every group's `span` — the groups
  have to tile it exactly, no gap or overlap.

To try it: a companion preset,
[`examples/memory-cards/pc1500-maxed-out.pc1500`](../examples/memory-cards/pc1500-maxed-out.pc1500),
loads this card via `modulespecfile:` and prints `MEM` so you can see the
extra room BASIC gets. Open it via **Open Preset…**.

When you're ready to write your own: copy the closest bundled example
under `Qt6/resources/cards/` (`ce155.card.yaml` for a simple unbanked,
OR'd-strobe card; `ce1638.card.yaml` or `ce1601m.card.yaml` for a banked
one) as a starting point rather than starting from the reference docs
cold, and check compatibility/terminology names in the format doc's
table as you go.

### Preloaded-data cards & Dump Card YAML

A `.card.yaml` region's `initial-content:` can bake in starting bytes —
useful for shipping a card that already has programs or a filesystem on
it, rather than an empty one. The practical way to produce that content
is round-trip, not hand-typed:

1. **Prepare the card.** Run a preset that boots the right machine, plugs
   in the module, and drives it through whatever setup is needed
   (formatting, loading programs, writing config files). See
   [`examples/setup/`](../examples/setup/) for worked "preparation preset"
   examples.
2. **Dump it.** With that card still attached, click the Debug panel's
   **Dump Card YAML** button. It prints the card's current contents as
   an `initial-content:` block in the `addressed-hex` dialect — a format
   designed to be pasted straight in with no reformatting.
3. **Fold it in.** Paste that block into the target `.card.yaml`
   definition's matching region. A fresh card built from that definition
   now powers up already populated.

Alternatively, save the prepared card as a named battery-backed instance
(**Name & Save**, see [Saving your work](#saving-your-work-battery-backed-modules))
and reference it by name from a "real" preset — no `.card.yaml` editing
needed if you just want to reuse one prepared card rather than bake it
into a bundled definition.

### Preset files

A preset (`.pc1500`, `.pc1500a`, or `.pc1600`) scripts the emulator from
a cold boot: which machine and modules to use, then a sequence of
keystrokes and/or program loads to run. It's a restricted YAML dialect —
flat `key: value` mappings and `- key: value` list items only, no flow
style or inline comments.

Top level (the inline comments below are annotations for this guide only
— real preset files don't support inline `#` comments at all; a comment
must be on its own line):

```yaml
model: PC-1500              # PC-1500 | PC-1500A | PC-1600
firmware: A04                # PC-1500 only; A01 | A03 | A04

memory-expansion:            # PC-1500/1500A: one module in slot 1
  - modulespec: CE-155        #   by bundled/saved module-name
# - modulespecfile: path.card.yaml   #   or by definition file, relative to this preset

memory-expansion-1:           # PC-1600: slot 1
  - modulespec: CE-1600M
memory-expansion-2:           # PC-1600: slot 2
  - modulespec: CE-1601M

plotter: ce150                # ce150 (PC-1500/1500A) | ce1600p (PC-1600)
```

Then any number of `keys:`/`program:` blocks, run top to bottom:

```yaml
keys:
  - key: cl                   # press a named key (see the keyboard
  - type: PRINT"HELLO"        # section above for key names) or type a
  - wait: 2                   # line of BASIC (Enter is automatic);
  - wait:                     # `wait: N` waits N seconds of emulated
  - trace: run1.bin            # time; a bare `wait:` blocks until the ROM's
                                # idle loop re-engages (a long program/plot
                                # has finished); `trace:`/`trace: off`
                                # starts/stops a CPU trace file.

program:
  format: basic-text           # basic-text | basic-binary | binary
  path: myprogram.bas          # (basic-binary/binary; basic-text can
                                # also give `text: |` inline instead)
  address: 0x7000              # binary only: load address
```

- `format: basic-text` types the program in through the ROM's own line
  editor — slow but exact.
- `format: basic-binary` (alias `basic-tokenized`) tokenizes a plain-text
  BASIC listing and pokes it straight into the program area — much
  faster; the machine must be left in a loadable state first (typically
  a preceding `keys:` block with `- key: cl` / `- type: NEW0`).
- `format: binary` pokes raw machine code at `address` (PC-1500/1500A),
  or into a given `slot: S0|S1|S2` (PC-1600).

See [`examples/`](../examples/) for real preset files covering all of
the above, and `examples/memory-cards/pc1500-maxed-out.pc1500` for the
`modulespecfile:` form used with a custom card.
