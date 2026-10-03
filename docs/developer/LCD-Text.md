# Reading the LCD as text

`Core/Display/LcdText` turns the display into lines of text. A test, a
preset or a debugger client can then check a result directly, without
rendering a PNG and reading the image.

It is an exact lookup, not OCR. In text mode both machines print on a fixed
grid of 6-dot cells, 26 per row. The emulator has every pixel, and the
font comes from the machine's own ROM. So each cell either is exactly one
character, or it isn't text.

## Where to use it

| Where | How |
|---|---|
| Headless CLIs | `pc1500_cli` / `pc1600_cli --preset <file> --lcd-text <out.txt>` (or `-` for stdout), next to `--lcd-png`. It writes the rows, then `status: …` and, if any, `unparsed: n`. |
| Presets | `- expect: <text>` fails the preset unless one row contains `<text>`; the error shows the whole screen. Each step's log line carries `lcd=["row", …]` (and `input="…"`, the ROM's input-line buffer). |
| Debug server (running app) | The `calcu1600/screen` request returns `{rows, status, unparsed, poweredOn, cursor?: {row, col}}`. It needs a machine, not a debug session. |
| Tests | `pc1500LcdText(machine)` / `pc1600LcdText(machine)` (`Core/PC1500/PC1500LcdText.hpp`, `Core/PC1600/PC1600LcdText.hpp`); see `Core/tests/lcd_text_tests.cpp`. |

Example, the plain PC-1600 after `PRINT MEM`:

```
PRINT MEM
                     11834


status: DEG RUN I
```

## What a cell becomes

Each cell is checked in this order:

1. **All dots off:** a space.
2. **Cursor:** the cursor cell, while the ROM reports a cursor. Its
   character is decoded from the ROM's save cell, because the ROM draws the
   cursor *over* the cell (see below).
3. **Glyph:** a glyph of the font. If two codes share a glyph, the one added
   first wins, which is printable ASCII.
4. **Reverse:** an inverted glyph. It is counted in `reverseCells`.
5. **Not text:** anything else, written as U+FFFD (`�`) and counted in
   `unparsed`.

Not-text cells are graphics (`LINE`, `PSET`, `GPRINT`), user-defined
characters, or text drawn off the grid. When `unparsed` is above 0, read the
PNG instead.

**Encoding:**
- Printable ASCII appears as is, except that `\` is written `\\`.
- Any other code the font has (katakana, graphics characters, 7FH) is
  written `\xHH`.
- Trailing spaces are dropped from each row.

`expect:` compares against this text.

**Plain text (Copy Screen):** `LcdText::plainRows` / `plainText()` hold the
same screen for reading. Each glyph carries its own Unicode text
(`LcdGlyph::text`), which the font builder takes from the ROM shape:
- PC-1600: ASCII, 7FH as █, `CGHIGH` as CP437 (all 128 shapes checked in
  both ROMs), and `CGSPEC` as □ √ π (the fourth sign has no counterpart, so
  it is a blank).
- PC-1500: ASCII, except where `CHARSET` draws another sign: 27H □, 5BH √,
  5CH ¥, 5DH π, 7FH █.

There are no escapes, and not-text cells are blanks. `plainText()` drops
trailing empty rows and is empty when no cell is text (`parsedCells == 0`)
or the display is off. The tables are in `Core/Display/LcdCharsets.hpp`,
including `jisX0201Kana()` for a future kana font.

**Codes that look the same:**
- On the PC-1500, 60H (backtick) has a blank glyph, so it reads as a space.
- On the PC-1600, FFH is blank too.

## Fonts

Each font is read from the loaded ROM, so every ROM version uses its own.

| Machine | Font | Cell |
|---|---|---|
| PC-1500/1500A | `CHARSET` at FCA0H, 5 bytes per character, 20H–7FH. `CHAR_2_ADDR` (EE48H) computes FC00H + 5 × code, and `CHAR_OUT_2` (ED5BH) adds a blank sixth column. | 6 × 7 |
| PC-1600 | Bank 6, column-wise, 6 bytes per character: `CGLOW` 20H–7FH, `CGHIGH` 80H–FFH, and the PC-1500-style `CGSPEC` cells for 1EH/27H/5BH/5DH. | 6 × 8 |

The PC-1600 font is at different addresses in the two ROM versions
(`CGLOW`: new ADA4H, old ADE3H). The parser finds it through the
jump-table entry at 8030H: that entry leads to `CGSET80`, whose
`ld de,CGHIGH` (+4) gives `CGHIGH`. `CGLOW` sits 60H × 6 bytes before
`CGHIGH`, and `CGSPEC` 4 × 6 bytes before `CGLOW`.

The PC-1500's second character set (codes 80H and up, through
`KATACHAR`) is not in the ROM. Neither are PC-1600 user character sets
(`CGSET80` with DE ≠ 0, `UPACGA`). Characters from either read as not
text.

## Cursor

- **PC-1600:** `CRSRDRAW` (bank 6, 8543H) saves the cell in `CRSRSAV`
  (F069H), then overwrites it with five columns of 40H (underline,
  `CRSRST` 1) or 7FH (block, `CRSRST` 2) and a blank sixth column. The
  cursor is shown while `LCDWK2` (F05EH) bit 0 is set, at `CRSRY`/`CRSRX`
  (F05FH/F060H). The underline is exactly the `_` glyph, so the parser
  checks only that one cell.
- **PC-1500:** `WAIT_4_KB` (E315H) blinks the cell between glyph 7FH and
  the character saved in `BLNKD_CHAR_CODE` (787DH). `CURSOR_BLNK` (787CH)
  is 81H while the block is shown. Its position is kept only as a
  display-RAM address, so the first 7FH cell is taken as the cursor.

## Status symbols

The status line is not part of the bitmap, so the model adapters list the
lit symbols separately. Both models use the same model and names
(`Core/Display/StatusLine.hpp`), since their annunciator bytes are the same
bit for bit:

BUSY SHIFT S ROMAJI KANA SMALL DEG/GRAD/RAD RUN PRO RESERVE DEF I II III CTRL BATT

- The PC-1600 adds S, ROMAJI and CTRL.
- BATT is the low-battery warning, so it never shows on the PC-1500, whose
  analog meter has no data bit.
- The DE, G and RAD segments are joined the way the panel reads them: DE+G
  = `DEG`, G+RAD = `GRAD`, RAD alone = `RAD`.

On the PC-1600, the LH5803's writes to the PC-1500 display RAM, DEGREE/
RADIAN/GRAD among them, reach the LCD at once through the gate array's
mirror; see [PC1600-Core-Limitations.md](PC1600-Core-Limitations.md).
