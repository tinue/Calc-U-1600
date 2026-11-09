# Plan: write-protect switch for RAM modules, CE-161 battery-backed + switch

> **Status (2026-11-09): implemented.** Differences: the live switch is
> flipped through `MachineSession::setSlotWriteProtected()` (ModuleBay
> only holds a const card); the headless `savePresetMedia()` flips it
> too. The end-to-end test fills the module from the harness instead of
> running the manual's BASIC fill loop, and runs on the PC-1500 and the
> PC-1500A (RAM end &58).

## Context

The real CE-161 is 16 KB of battery-backed SRAM at &0000–&3FFF with a switch in the RAMs' R/W line.
In "Setting" writes are blocked, so the PC-1500 power-on scan (`RESET_9`, A04 E098) finds page 0
not writable, sees the &55 header the user POKEd, and treats the module as a ROM-like program module
(program at &0100, reserve area in the module, RAM BASIC at &4000). The emulator models it as
plain, non-battery RAM, so neither the switch nor the persistence exists.

Decided with the user:
- The switch belongs to the generic RAM content model and is part of the saved card file, so a
  template can ship protected (with header + program) or writable.
- GUI: a "write-protected" checkbox in the Name & Save dialog, shown only for cards with a switch.
  Name & Save stays **templates-only** (no change to `canNameAndSave`). Once saved protected, the card
  stays protected; there is no UI to flip it back.
- Presets get an option to save protected too.

## Format change (pre-1.0, breaking allowed)

`regular` content's unused `write-protect: { default, persisted }` (no bundled card uses it; only
parsed in `Core/Connector/MemoryCardDefinition.hpp:645` and read once in
`SoftwareDefinedCard.hpp:97`) becomes a scalar switch position, always part of the file:

```yaml
content:
  kind: regular
  write-protect: unprotected   # protected | unprotected; key present = the module has a switch
```

`RegionContent`: replace `hasWriteProtect / writeProtectDefaultProtected / writeProtectPersisted`
with `hasWriteProtect` + `writeProtected` (initial position). The old map form is rejected as invalid.

## Steps

1. **Core: card model** (`Core/Connector/MemoryCardDefinition.hpp`, `SoftwareDefinedCard.hpp`,
   `ExpansionCard.hpp`)
   - Parser: scalar `write-protect`; error text names `protected|unprotected`.
   - `SoftwareDefinedCard`: a runtime `m_writeProtected` initialised from the definition; the
     write path (line ~97) refuses when a region with a switch is protected, also for `pins.direct`
     writes (debugger poke / preset loader), so it behaves like a protected chip.
     `debugMemories()` reports such a region as `Rom` while protected.
   - `CardBase`: `virtual bool hasWriteProtectSwitch() const { return false; }`,
     `virtual void setWriteProtected(bool)`, `virtual bool writeProtected() const`; implemented by
     `SoftwareDefinedCard`. Toggling it bumps nothing in content (contents unchanged).

2. **Core: saving** (`Core/Connector/BatteryCardInstance.hpp`, `MediaSave.hpp`)
   - `spliceBatteryCardInstance(..., std::optional<bool> writeProtect = {})`: when set, rewrite the
     value of the existing `write-protect:` line in the region's content block (refuse with an error
     if the card has no such line). Unset = leave the file's line as is (autosave keeps the state).
   - `formatCardSave(...)` passes it through.

3. **Shared: ModuleBay** (`Shared/ModuleBay.hpp/.cpp`)
   - `bool canWriteProtect(int slot) const` = live card `hasWriteProtectSwitch()` and not already
     protected (for showing the checkbox).
   - `nameAndSave(slot, name, bool writeProtect, error)` and `saveAsFromPreset(..., bool writeProtect)`
     → `saveSlotAs` passes `writeProtect ? optional(true) : nullopt` to `formatCardSave`; after the
     file is written, call `card->setWriteProtected(true)` on the live card so writes stop at once
     (the ROM picks it up at the next OFF/ON, exactly like the manual's procedure).
   - Refuse `writeProtect` for a card without a switch ("<name> has no write-protect switch.").

4. **Presets** (`Core/Preset/PresetFile.cpp/.hpp`, `PresetSaveAsRequest`, Qt/headless handlers in
   `Qt6/app/MemoryModuleManager`, `Core/Preset/PresetMedia.hpp`)
   - Syntax: `- saveas: live|template protected slot-1:<name>` (keyword after the mode, so names
     with spaces stay unambiguous). `protected` with `floppy:` is a parse error.
   - New `PresetStep::saveAsProtected` / `PresetSaveAsRequest::writeProtect`.

5. **Qt GUI** (`Qt6/app/MainWindow.cpp:127`, `MemoryModuleManager`)
   - Replace `QInputDialog::getText` with a small `QDialog` (name line edit + checkbox
     "Write-protect (switch set to read-only — cannot be undone)"), checkbox shown only when
     `canWriteProtect(slot)`. Keep the dialog in `Qt6/app/` (display only; rules stay in ModuleBay).

6. **Web GUI** (`Web/bindings.cpp:406,743`, `Web/src/CalcU1600.jsx:199`, dialogs)
   - Slot info gets `canWriteProtect`; `nameAndSaveCard(slot, name, writeProtect)`; the prompt dialog
     gets an optional checkbox.

7. **CE-161 card** (`Qt6/resources/cards/ce161.card.yaml`)
   - `battery: true`, `write-protect: unprotected`; rewrite the header comment: Y0 window &0000–&3FFF,
     schematic (eight 2 KB chips, '138 on AD11–13, Y0 on G2A, DME0 on G1; switch in R/W), the
     &55 header / `POKE 0,85,0,1,0,0,0,0,255` / `NEW 256` workflow in two or three lines.

8. **Docs**: `docs/Memory-Card-Definition-Format.md` §4 + the CE-161 row, `docs/Memory-Card-Definition-Spec.md`
   §5/§8 (settle the open question: switch position is part of the file), `docs/User-Guide.md`
   (CE-161 row: battery yes; how to make a protected CE-161; `saveas: … protected`), `CHANGELOG.md`.
   Add the CE-161 ROM analysis (header layout, memory maps, POKE) as
   `docs/background/CE-161-Program-Module.md` + row in `docs/background/README.md`; commit this plan as
   `docs/background/plans/RAM-Write-Protect-Plan.md`. Decisions.md: protected writes are refused even
   for debugger/loader direct writes (deliberate).

## Tests

- Definition parse: scalar accepted, map form and bad values rejected (existing card-definition tests).
- `SoftwareDefinedCard`: writes taken when unprotected, refused (incl. direct) when protected;
  `setWriteProtected` switches at runtime.
- Splice: `write-protect:` value rewritten; untouched without the option; error without the key
  (`Core/tests/battery_card_instance_tests.cpp`).
- ModuleBay / AppSession (`Shared/tests/shared_app_session_tests.cpp`): Name & Save with
  writeProtect writes `protected`, live card refuses writes, reloading the instance comes up protected;
  card without a switch is refused.
- Preset parse: `live protected slot-1:X` ok, `protected floppy:` rejected.
- **End-to-end** (`Core/tests/ce161_tests.cpp`, PC-1500): CE-161 attached → `NEW 256`, enter a short
  program, run the manual's fill program, `POKE 0,85,0,1,0,0,0,0,255`; set protected; OFF/ON →
  `PEEK &7860` = 0, `PEEK &7863` = &40, `NEW 0`, MEM = 2047-ish, the module program still runs by
  its label, and a POKE into &0100 has no effect.

## Verification

- `tools/run_tests.sh`, `tools/build_wasm_check.sh`, Qt app build (`tools/build_app.sh`),
  `Web/build.sh` + `Web/run-tests.sh`.
- Manual GUI check: select CE-161 template → prepare as above → Name & Save with the checkbox →
  OFF/ON → "NEW0?:CHECK" → `NEW 0` → module program runs; quit/restart and reselect the instance →
  still protected with its contents.
- Commit per phase on `dev-0.8.1`.
