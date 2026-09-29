# examples/setup

See also `docs/User-Guide.md`'s "Preloaded-data cards & Dump Card YAML"
section for a narrative walkthrough of the workflow below.

These files are not demos to look at — they are **preparation scripts**. Each one
drives the emulator through the steps needed to get a memory card (or expansion
module) into a known, useful state: format it, split it as a RAM disk, load a set
of BASIC programs, write a config file, and so on.

## Workflow

1. Run one of these preset files. It boots the right machine with the right
   module fitted and types/loads everything into the card.
2. Once the card is set up, do one of two things with it:

   - **Save the card.** End the preset with `- saveas: template|live ...`: by
     name into your save folder, or as a file next to the preset
     (`file:`), which a "real" preset then loads with `- modulespecfile:` /
     `floppy-file:`. `examples/dwx/make_diskworks_media.pc1600` does this for
     `examples/dwx/DiskWorks.pc1600`.

   - **Dump the card.** Use the debug **"Dump Card YAML"** action to emit a card
     definition, and fold the pre-defined data it captures into one of the
     `*.card.yaml` files in `Calc-U-1600/Resources/` (e.g. `ce1601m.card.yaml`).
     That bakes the prepared contents into the shipped resource so a fresh card
     comes up already populated.

## Contents

| File | Prepares |
| --- | --- |
| `ce1638_bankswrm.pc1500` / `.pc1500a` | PC-1500 with a real CE-1638 128K module, bank-switch test loaded |
| `firmware_bootstrap_util*.pc1500a` | Firmware bootstrap / update utilities staged into a module |
| `update.bas`, `updaterm.bas`, `util_*.bas`, `utilrm_*.bas` | BASIC sources loaded by the bootstrap presets |
| `ce1638_bankswrm_fixed.bin` | Machine-code payload for the CE-1638 bank-switch test |
