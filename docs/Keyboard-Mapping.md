# Host Keyboard Mapping

The six-key block above the cursor keys (extended / PC-style keyboard):

| | | |
|:---:|:---:|:---:|
| &nbsp;<br>▲▼ | `CTRL` ¹<br>`RCL` | `DEF`<br>`DEL` |
| `CA`<br>`CL` | `KB II` ¹<br>`SML` | &nbsp;<br>`INS` |

Top line: with host Shift held; bottom line: plain.

¹ PC-1600 only; on the PC-1500/1500A Shift is ignored on this key.

**Tab** → `MODE` (RUN/PRO toggle). **Shift** tapped on its own → `SHIFT` (latched).

---

How host keys map to calculator keys in the Qt6 app
(`Qt6/app/PC1500KeyboardMap.cpp`, shifted symbols from
`Core/SharpShiftedSymbols.hpp`). "Shift+`X`" in the calculator column
means the calculator's own SHIFT is tapped first, then `X`.

## Special keys

| Host key | PC-1600 | PC-1500 / 1500A | Notes |
|---|---|---|---|
| Return / Enter | `ENTER` | `ENTER` | |
| Space | `SPACE` | `SPACE` | |
| ↑ ↓ ← → | `↑` `↓` `←` `→` | `↑` `↓` `←` `→` | Host Shift ignored |
| Backspace | `BS` | `←` | Host Shift ignored |
| Tab | `MODE` | `MODE` | |
| F1 – F6 | `F1` – `F6` | `F1` – `F6` | Host Shift ignored |
| Forward Delete | `CL` | `CL` | Mac laptop: fn+Delete |
| Shift+Forward Delete | Shift+`CL` | Shift+`CL` | |

## Navigation cluster (external / PC-style keyboard)

Project's own choice, not a hardware correspondence.

| Host key | PC-1600 | PC-1500 / 1500A | Notes |
|---|---|---|---|
| Scroll Lock | `RSV` | `RSV` | macOS: matched by virtual key 0x6E (Mac-layout keyboard) |
| Insert | `RSV` | `RSV` | Windows/Linux only; on macOS Insert arrives as Help and is unmapped |
| Home | `RCL` | `RCL` | |
| Shift+Home | `CTRL` | `RCL` | PC-1500 has no CTRL key |
| End | `SML` | `SML` | |
| Shift+End | `KB II` | `SML` | PC-1500 has no KB II key |
| Page Up | Shift+`←` | Shift+`←` | |
| Shift+Page Up | `DEF` | `DEF` | |
| Page Down | Shift+`→` | Shift+`→` | |

## Characters

Resolved from the character the host key types, so they follow the host
keyboard layout.

| Typed character | PC-1600 | PC-1500 / 1500A |
|---|---|---|
| `a`–`z`, `A`–`Z` | letter key (case ignored) | letter key (case ignored) |
| `0`–`9` | digit key | digit key |
| `+ - = * / ( ) .` | same key | same key |
| `!` | Shift+`F1` | Shift+`F1` |
| `"` | Shift+`F2` | Shift+`F2` |
| `#` | Shift+`F3` | Shift+`F3` |
| `$` | Shift+`F4` | Shift+`F4` |
| `%` | Shift+`F5` | Shift+`F5` |
| `&` | Shift+`F6` | Shift+`F6` |
| `<` | Shift+`(` | Shift+`(` |
| `>` | Shift+`)` | Shift+`)` |
| `?` | Shift+`/` | Shift+`/` |
| `:` | Shift+`*` | Shift+`*` |
| `,` | Shift+`-` | Shift+`-` |
| `;` | Shift+`+` | Shift+`+` |
| `@` | Shift+`=` | Shift+`=` |
| `^` | Shift+`SPACE` | — |
| `'` | Shift+`1` | — |
| `[` | Shift+`2` | — |
| `]` | Shift+`3` | — |
| `` ` `` | Shift+`4` | — |
| `{` | Shift+`5` | — |
| `}` | Shift+`6` | — |
| `\` | Shift+`7` | — |
| `~` | Shift+`8` | — |
| `_` | Shift+`.` | — |
| `\|` | Shift+`0` | — |

## Accented characters (PC-1600)

The PC-1600's `KB II` key latches, and while it is on the letters and
`( )` give the characters below (ROM tables `KYCDKB2` / `KYCDSK2`, bank 6
9592H / 95E5H; CP437 codes). A host character from this list is typed as
`KB II`, [`SHIFT`,] key, `KB II`.

| Key | KB II | SHIFT+KB II | | Key | KB II | SHIFT+KB II |
|---|---|---|---|---|---|---|
| A | á | á | | N | £ | ¥ |
| B | ù | û | | O | Ñ | ñ |
| C | ì | î | | P | Ç | ç |
| D | í | í | | Q | Ä | ä |
| E | ï | ï | | R | Ö | ö |
| F | ó | ó | | S | É | é |
| G | ú | ú | | T | Ü | ü |
| H | ¡ | ½ | | U | Æ | æ |
| I | Å | å | | V | ò | ô |
| J | ¿ | ¼ | | W | ë | ë |
| K | ª | ⌐ | | X | è | ê |
| L | º | ¬ | | Y | ÿ | ÿ |
| M | ¢ | ¢ | | Z | à | â |
| ( | ₧ | « | | ) | ƒ | » |

- **Host keys, paste and preset `type:`** all keep the case: `ä` gives
  `ä`, `Ä` gives `Ä`. This is unlike the plain letters, which the host keys
  type in uppercase. An uppercase the calculator lacks (`Ë`, `Û`) gives its
  lowercase.
- With `SHIFT` latched on the calculator, it is released first and the
  character is typed as usual. With `KB II` latched, an accented host key
  does nothing: press the letter itself instead (`R` gives `Ö`).
- `SML` doesn't change these characters.
- Dead keys compose as usual (`¨` then `U` gives `Ü` on a Swiss or German
  layout, Option-U then U on a US Mac): the app takes part in the OS input
  method and types the composed character.

## Modifiers

- **macOS:** anything held with Cmd or Control never reaches the
  calculator (shortcuts). Option still types the character it produces.
- **Windows/Linux:** anything held with the Windows/Super key, or with
  Ctrl or Alt alone, never reaches the calculator. Ctrl+Alt together
  (AltGr) still types the character it produces.
- **Host Shift tapped on its own** — pressed and released within 0.4 s,
  with no other key or mouse click in between — taps the calculator's
  `SHIFT`, latching it for the next key. Shift held while typing another
  key only acts as a modifier.

## Not mapped

`ON` and `OFF` have no host key; use the on-screen faceplate.
