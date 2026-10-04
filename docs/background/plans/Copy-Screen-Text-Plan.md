# Copy Screen: the LCD as text next to the bitmap

## Context

Edit ▸ Copy Screen (⌘C) currently puts only the LCD dot matrix on the clipboard
(`MainWindow::copyScreenToClipboard`, `Qt6/app/MainWindow.cpp:735`). The new LCD text
analyzer (`Core/Display/LcdText.{hpp,cpp}`, `MachineController::lcdText`) can read the
screen as characters, so Copy Screen should also put readable text on the clipboard:
pasting into a text editor gives the text, into an image app the bitmap.

Calc-U-59's printer copy (`~/Development/public/Calc-U-59/App/Views/PrinterView.swift`
`copyBoth()`, ~l.209) is the model: one `clearContents`, then `writeObjects([image,
text])` — image first so it is the primary item, plain text = lines joined with `\n`.

User decisions:
- Readable text only: CP437 glyphs and katakana as their Unicode characters, no escaping;
  an unparsable cell (graphics, redefined character) becomes a blank.
- No text at all when nothing could be parsed (full graphics screen, blank screen, power off).
- Rows only, no status symbols.
- Katakana: mapping only, no new parsing (no font that the parser reads has kana today).

## Findings that shape the design

- The `rows` encoding (`\\`, `\xHH`) stays as is — it is the `--lcd-text` / `expect:` /
  DAP format.
- The *code* alone is not enough for plain text: the PC-1600 font adds CGSPEC glyphs
  (bank 6, the 4 × 6 bytes before CGLOW) under codes 1EH/27H/5BH/5DH, and their shapes
  are □ / √ / π / … — not `'`, `[`, `]`. The PC-1500 CHARSET (FCA0H) draws 27H as □,
  5BH as √, 5CH as ¥, 5DH as π, 7FH as █. So each glyph needs its own Unicode text,
  assigned by the font builder that knows the ROM.
- PC-1600 bank 6 CGHIGH (80H–FFH) is the CP437 upper half in both ROM revisions (checked
  80H Ç, 8EH Ä, B1H ▒, C4H ─, E3H π, FBH √ …). Its 7FH is a full block (█), not CP437 ⌂.
- The kana set (PC-1500 KATACHAR / PC-1600 PC-1500 mode, LH5803 ROM C700H/C6BDH) is
  JIS X 0201 for A1H–DFH; 80H–A0H and E0H–E5H are kanji/symbols.

## Changes

### 1. Core: per-glyph Unicode in the font, plain rows in the result

`Core/Display/LcdText.hpp/.cpp`
- `LcdFont::glyphs` entry gains the glyph's UTF-8 text (a small `LcdGlyph { uint8_t code;
  LcdCell cell; std::string text; }` replacing the pair — update `tinyFont()` in
  `Core/tests/lcd_text_tests.cpp` and both builders). Empty `text` → blank in plain text.
- `parseLcdText` builds, next to `rows`, `plainRows`: per cell the matched glyph's `text`
  (also for the reverse-video and cursor-under paths), a space for blank and unparsed
  cells; trailing spaces trimmed like `rows`. It also counts `parsedCells` (non-blank
  cells that matched a glyph).
- `LcdText::plainText()`: empty if `!poweredOn` or `parsedCells == 0`; otherwise
  `plainRows` joined with `\n`, trailing empty rows dropped.
- Update the header comment block (encoding section) to describe both forms.

`Core/Display/LcdCharsets.hpp` (new, model-free)
- `kCp437High[128]` (char32_t, the standard IBM CP437 80H–FFH table, same as libsharpdx
  `src/cp437_tables.rs`), `jisX0201Kana(code)` (A1H–DFH → U+FF61–FF9F), and a small
  `utf8(char32_t)` encoder.

Font builders:
- `Core/PC1600/PC1600LcdText.hpp` `pc1600LcdFont`: 20H–7EH ASCII, 7FH █, 80H–FFH from
  `kCp437High`, CGSPEC 1EH/27H/5BH/5DH by their drawn shapes (□, √, π, and the 4th
  after looking at it; empty if no clear Unicode). Comment cites the ROM addresses.
- `Core/PC1500/PC1500LcdText.hpp` `pc1500LcdFont`: ASCII with the CHARSET overrides
  27H □, 5BH √, 5CH ¥, 5DH π, 7FH █ (verify each shape against FCA0H while writing).
- Kana: `jisX0201Kana` is ready for a future kana font; add a TODO.md entry "LCD text:
  parse the kana set (PC-1500 KATACHAR via CHAR_2_ADDR EE5AH; PC-1600 PC-1500 mode
  C700H/C6BDH)".

### 2. Qt: write image + text

`Qt6/app/MainWindow.cpp` `copyScreenToClipboard`
- Get `LcdText` via `m_controller->lcdText()`; `text = QString::fromStdString(t.plainText())`.
- macOS: `macSetClipboardImage(image, widthPt, heightPt, text)` — extend
  `Qt6/app/MacClipboardImage.{h,mm}` with an optional `const QString& text`: after the
  NSImage `writeObjects` and PNG `setData`, `setString:forType:NSPasteboardTypeString`
  when non-empty (image stays primary, as in Calc-U-59).
- Other platforms: `QMimeData` with `setImageData(image)` + `setText(text)` (if any) →
  `clipboard()->setMimeData`.
- Menu label stays "Copy Screen".

Note: after Copy Screen, Edit ▸ Paste Text now finds text and types the screen back in.
That is the natural round trip; document it, don't block it.

### 3. Docs

- `docs/User-Guide.md` §"Copy Screen and Paste Text" (~l.130): text is added, CP437 /
  katakana as Unicode, graphics as blanks, no text for a graphics-only screen.
- `docs/developer/LCD-Text.md`: the plain-text form and per-glyph Unicode.
- `docs/background/Decisions.md`: "Clipboard text is plain Unicode while `--lcd-text`
  escapes (`\\`, `\xHH`, U+FFFD) — the latter must be unambiguous for tests."

## Verification

- Unit tests (`Core/tests/lcd_text_tests.cpp`): `plainRows`/`plainText` for the tiny font
  (`\` stays `\`, a high-code glyph gives its text, unparsed → space, all-unparsed →
  empty, power off → empty); booted PC-1600: a `PRINT CHR$(&8E);CHR$(&B1);"\"` screen
  gives `Ä▒\`; booted PC-1500: π / √ / ¥ glyphs; `jisX0201Kana(0xB1) == ｱ`. Build and run
  the full Core test suite.
- GUI (launch per memory: `-ApplePersistenceIgnoreState YES`, quit via `calcu1600/quit`):
  boot PC-1600, show text, ⌘C; check `pbpaste` shows the rows and
  `osascript -e 'clipboard info'` lists both the image and text flavours; paste into
  Preview still gives the physical-size image. Repeat with a GPRINT/graphics screen →
  `pbpaste` empty, image present. Same on PC-1500.
