# PC-1600: type KBII accented characters from the host

## Context

The PC-1600's KBII key latches, like SML. While it is on, letters and `(` `)` produce the
"international" characters. None of the three host entry paths can produce them today:
- **Live host keyboard:** the Qt key map turns `ü` into an unknown key name.
- **Edit > Paste:** `buildPasteSteps` drops every non-ASCII byte.
- **Preset `type:`:** `typeLine` stops with "no PC-1600 key". This path is also used by
  `pc1600_cli`, the GUI's preset load and the debugger's `typeCommand`. `.bas` loading is not
  affected, because it goes through libsharpdx.

Every accented character is typed as a sequence that closes itself:
**KBII, [SHIFT,] key, KBII**. The paths differ only in how they choose between the KBII and
SHIFT+KBII variant. This follows the existing ASCII behaviour:

| Path | ASCII today | Accented (new) |
|---|---|---|
| Live host keyboard | case-folded: `g` and `G` both press G and give `G` | case-folded: `é` and `É` both give **É** (KBII S) |
| Paste, `type:` | case-exact: `g` → SHIFT G → `g` | case-exact: `é` → KBII SHIFT S → **é**, and `É` → KBII S → **É** |

So typing `mérCi` on the host keyboard gives `MÉRCI`, while pasting it or using
`type: mérci` gives `mérci`.

## The ROM tables (found)

`disasm/new/PC1600-P2-B6.asm` (bank 6). `KEYNORM_B6` (94D4H) points the RAM table pointers
at them. The OLD revision has identical tables, 7 bytes lower.
- `KYCDKB2` (9592H) is the KBII table and `KYCDSK2` (95E5H) the SHIFT+KBII table. Both are
  indexed by key code − 08H.
- `KEYCONV` (P1-B3 48A7H) checks KBII **before** the SML/caps test. With KBII on, SHIFT
  selects `KYCDSK2`, otherwise `KYCDKB2` is used. SML therefore has no effect: the result is
  deterministic.
- The code points follow the CP437 layout, which the CE-1600P notes and libsharpdx already use.

| Key | KBII | SHIFT+KBII | | Key | KBII | SHIFT+KBII |
|---|---|---|---|---|---|---|
| A | á A0 | á | | N | £ 9C | ¥ 9D |
| B | ù 97 | û 96 | | O | Ñ A5 | ñ A4 |
| C | ì 8D | î 8C | | P | Ç 80 | ç 87 |
| D | í A1 | í | | Q | Ä 8E | ä 84 |
| E | ï 8B | ï | | R | Ö 99 | ö 94 |
| F | ó A2 | ó | | S | É 90 | é 82 |
| G | ú A3 | ú | | T | Ü 9A | ü 81 |
| H | ¡ AD | ½ AB | | U | Æ 92 | æ 91 |
| I | Å 8F | å 86 | | V | ò 95 | ô 93 |
| J | ¿ A8 | ¼ AC | | W | ë 89 | ë |
| K | ª A6 | ⌐ A9 | | X | è 8A | ê 88 |
| L | º A7 | ¬ AA | | Y | ÿ 98 | ÿ |
| M | ¢ 9B | ¢ | | Z | à 85 | â 83 |
| ( | ₧ 9E | « AE | | ) | ƒ 9F | » AF |

KBII+RCL gives BBH. RCL is a function key, not a character, so it is left out.

**Resolution rules.** One table, generated from the two ROM columns: each character maps to
`(key, shift)`. A character in both columns (á í ï ó ú ë ÿ ¢) is taken without SHIFT.
- **Exact (paste, `type:`):** look up the character itself. If it is not in the table but
  its case partner is, use the partner. This covers uppercase forms the ROM does not have:
  `Ë` → ë (W), `Û` → û (SHIFT B), `Ù` → ù (B).
- **Folded (live keys):** for a character whose upper and lower case are both in the table
  (the 8 pairs Ä Ö Ü É Ç Å Æ Ñ), always use the unshifted uppercase one. Everything else
  resolves as in the exact rule, with no smart handling. For example `ë`/`Ë` → KBII W, and
  `û`/`Û` → KBII SHIFT B, since SHIFT+KBII is the only way to get û.

## Batching `öäü` under one KBII latch: not worth it

- **Gain:** two taps, about 16 frames or 0.27 s, per *adjacent* accented character. This
  applies to paste and `type:` only. Live keys arrive one at a time, so they gain nothing.
  Adjacent runs are rare in real text: Grüße, Café and Ölförderung all alternate with plain
  letters.
- **Cost:** latch state would have to be tracked in `KeyPasteFeeder::append`, `typeLine`
  and the live GUI path. KBII would also have to be released on every exit path: an
  unsupported character, a cancelled paste, a typing error. Sequences that close themselves
  always leave KBII off, which keeps the next key correct.
- If long accented runs ever matter, the batching could be added to `append()` alone.

## Implementation

1. **`Core/Utf8.hpp` (new, small):** `bool decodeUtf8(const std::string&, size_t& i, char32_t& cp)`.
   Core has no UTF-8 decoder yet. On an invalid byte it skips that one byte.
2. **`Core/PC1600/PC1600TypedInput.hpp`:** add the table as rows
   `{char32_t ch, char32_t casePartner /*0 if none*/, const char* key, bool shift}`, with a
   comment citing `KYCDKB2`, `KYCDSK2` and `KEYCONV`. Add
   `pc1600ResolveKbiiChar(char32_t cp, bool foldCase, std::string* key, bool* needsShift)`,
   which implements both rules. Change `pc1600ResolveTypedChar` to take a `char32_t` and
   fill a `PasteStep` (key, needsShift, **needsKbii**). ASCII keeps the existing logic, and
   non-ASCII goes to `pc1600ResolveKbiiChar(cp, /*foldCase=*/false, …)`. The PC-1500
   resolver gets the same signature and returns false for non-ASCII.
3. **`Core/KeyPaste.{hpp,cpp}` (paste):**
   - Add `bool needsKbii` to `PasteStep` and update the `TypedCharResolver` signature.
   - `buildPasteSteps` decodes UTF-8. Control characters are still skipped.
   - `append()` emits `kbii, wait(shiftGap), [shift, wait(shiftGap)], key, kbii, wait(shiftGap)`.
   - `cancel()` keeps a pending closing KBII tap once its opening tap has been typed, so a
     cancelled paste never leaves KBII latched.
4. **`Core/PC1600/PC1600BasicTyper.cpp` `typeLine` (`type:`):**
   - Decode UTF-8 and type the same sequence with `tapKey("kbii")` + `kShiftGapFrames`.
   - Update the "Case:" paragraph in `PC1600BasicTyper.hpp`.
   - `kMaxBasicLineLength` counts code points instead of bytes.
   - No preset files change. This also covers `pc1600_cli` and `typeCommand` without
     further work.
5. **Qt `PC1500KeyboardMap` (live keys):** add `needsKbii` to `ResolvedKey`. For the PC-1600,
   when the key produces a single non-ASCII character of text, call
   `pc1600ResolveKbiiChar(cp, /*foldCase=*/true, …)` *before* `characterName`. Today
   `characterName` turns `ü` into the bogus key name "\xFC". macOS dead keys (⌥u u) already
   deliver the composed `ü`.
6. **Live GUI timing (`MainWindow::keyPressEvent`, `MachineController`):**
   - A KBII sequence takes about 0.4 s. A key typed during that time would land while KBII
     is still latched: the `r` in "für" would give `Ö`.
   - Add `MachineController::typeLiveStep(PasteStep)`. It appends to the frame-paced
     `m_paste` feeder and marks the feed as live (`m_liveTyping`).
   - `pasteActive()` is true only for a real paste, so a keystroke no longer cancels a live
     sequence. The new `liveTypingActive()` reports a live sequence. `pasteText` clears the
     live mark.
   - In the PC-1600 branch, if `needsKbii` **or** `liveTypingActive()`, call `typeLiveStep`,
     so later keys queue behind the sequence as taps. Otherwise keep the existing
     `tapShiftedKey` / `trackAndPress` path.
7. **Docs:**
   - `docs/Decisions.md`: a new entry stating that live keys fold case (host `é`/`É` → É)
     while paste and `type:` are exact, why SHIFT is used for the SHIFT+KBII-only
     characters, and why there is no batching.
   - `docs/Keyboard-Mapping.md`: the table.
   - `docs/User-Guide.md`: the typing and paste section, including the `ctrl`/`kbii`/`bs`
     note near line 648.

## Tests (`Core/tests/`)

- **`key_paste_tests.cpp` (exact rule):**
  - `é` → [kbii, shift, S, kbii]; `É` → [kbii, S, kbii]; `ë` and `Ë` → [kbii, W, kbii];
    `û` → [kbii, shift, B, kbii].
  - `ß` and `→` are skipped. In `mérci`, the plain letters keep their SHIFT.
  - A cancel in the middle of a sequence still taps the closing KBII.
- **Resolver unit tests for the folded rule:** `é`/`É` → S without shift; `û`/`Û` → B with
  shift.
- **`pc1600_basictyper_tests.cpp` (real ROM, PRO mode):**
  - `type 10 A$="mérci ÄäëûÜ«"`. The stored record must hold `m 82 r c i … 8E 84 89 96 9A AE`.
  - The KBII flag (F3C6H bit 7) must be clear afterwards.
  - The same line typed with SML locked must give the same accented bytes.
- If a Qt test for `PC1500KeyboardMap` exists, add PC-1600 cases `ü` → T+kbii and
  `û` → B+shift+kbii, and nothing for the PC-1500.

## Verification

- Build (`tools/build_pc1600_cli.sh`) and run the Core test suite.
- **Headless:** run a scratch preset in `headless/` with `type: PRINT "mérci Grüße"` through
  `pc1600_cli --preset`. The LCD should show `mérci Grüe`: `ß` has no key and is dropped.
- **GUI** (`tools/build_app.sh`, launched with `-ApplePersistenceIgnoreState YES`); the user
  checks these in the app:
  - Type `mérCi` and `für` quickly on the host keyboard → `MÉRCI`, `FÜR`.
  - Paste `mérci ÄÖÜ äöü ¿¡ ½` → the same text, exactly.
  - Afterwards the S/KBII indicator is off.
- Commit on `dev-0.6.0` in three groups: core + tests, GUI, docs.

## Addendum: latched SHIFT / KBII, type what's typed

Found in use: SHIFT latched on the calculator, then host `ö`, gave `R` and left KBII
latched. With SHIFT on, the KBII key toggles the key click (EDKBII P1-B0 6CB3H), so the
opening tap was lost and the closing tap latched KBII. A KBII sequence now reads the
latches when it starts (SYMB0 F64EH bit 1 SHIFT, STAT2 F3C6H bit 7 KBII):

| Latched | Sequence |
|---|---|
| nothing | KBII, [SHIFT,] key, KBII |
| SHIFT | SHIFT (un-latch), KBII, [SHIFT,] key, KBII |
| KBII, or SHIFT + KBII | nothing: ignored silently |

The case-folding of host keys was dropped at the same time: every path types what was
typed (`ö` gives ö, `Ö` gives Ö). The uppercase default of the host letters is for BASIC
keywords, and accented characters only appear in strings and REMs.
