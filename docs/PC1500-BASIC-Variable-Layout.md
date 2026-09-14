# PC-1500 BASIC variable/array RAM layout

How the PC-1500/1500A ROM BASIC stores variables in RAM. Confirmed
empirically against a real `PC-1500_A04.ROM` boot image; the byte offsets
here are that ROM revision's layout.

## Fixed numeric variables (`A`–`Z`)

208 bytes at `0x7900`–`0x79CF`, 26 slots of 8 bytes each. The slot for
letter *N* (A=1) is at `0x7900 + (N-1)*8`.

Each 8-byte slot is a BCD float, byte-identical to the documented
wire/exchange format:

```
byte 0: exponent (normalized: value = 0.d1d2...d10 * 10^exponent)
byte 1: sign (0x00 positive, 0x80 negative)
bytes 2-6: 5-byte packed BCD mantissa, 2 digits/byte, 10 digits total
byte 7: 0x00 terminator
```

Example: `05 00 10 00 01 00 00 00` → mantissa digits `1,0,0,0,0,1,0,0,0,0`,
exponent `5` → `0.1000010000 * 10^6 = 100001`.

## Fixed string variables (`A$`–`Z$`)

16 bytes/slot at `0x78C0`, position-indexed the same way as the numeric
slots. Left-aligned, null-padded ASCII — also byte-identical to the
documented wire/exchange format.

## Main-memory variables (multi-letter scalars, all DIM'd arrays)

These grow **downward from top-of-RAM**, tracked live by the `VAR_START`
pointer (`0x7899`/`0x789A`, big-endian). Each newly-created variable or
array is inserted *below* the previous ones — the earliest-created entry
ends up at the highest address, the most recent at `VAR_START`.

Per-entry format:

```
[name byte 1][name byte 2][4 header bytes][type/length byte][value data]
```

- **Name bytes** are literal ASCII. A single-letter main-memory scalar or
  array (no second letter) stores `0x00` + a flag addition in the second
  byte instead: `+0x80` marks an array, `+0x20` marks a string.
  Two-letter names store both letters literally.
- **4 header bytes**, confirmed shape: `[0x00][type-const][dim1 size][dim2
  size]`. `dim2 size` is `0x00` for a 1-D array or plain scalar.
  Type-const `0x23` = numeric array, `0x21` = string array; a plain
  (non-array) scalar's header reads `00 0B 00 00` regardless of type.
- **Numeric data**: type byte `0x88`, then one 8-byte BCD slot per scalar
  or consecutive slots per array element, in index order.
- **String data**: one length byte (the `*length` from `DIM ... *n`, or a
  default if omitted), then that many elements, each `length` bytes,
  left-aligned and null-padded.
- **Array element order is row-major**: for a 2-D array, the first
  subscript varies slowest (`M(0,0), M(0,1), M(1,0), M(1,1)`, ...).

A scalar variable and an array sharing the same base name are genuinely
separate, non-aliasing storage — array-ness and string-ness are flag bits
on the stored name, not a different table for the same slot. Fixed
variables (`A`–`Z`, `A$`–`Z$`) and main-memory variables never alias each
other either, even when named the same letter.

## Keyboard SHIFT is a one-shot latch, not a held modifier

Relevant to typing any BASIC line containing a shifted character
(scripted presets, the physical keyboard, or a test typer): on the real
PC-1500 keyboard, `SHIFT` is a one-shot latch. Tapping `SHIFT`
(press+release) arms it; the *next* key produces its shifted character
regardless of whether `SHIFT` is still held, and the latch is then
consumed — it is never held down while the base key is pressed, and
tapping it again with no intervening key is the only way to cancel a
pending shift.

Base-key → shifted-character mapping:

| Base key | Shifted | Base key | Shifted |
|---|---|---|---|
| F1 | `!` | `(` | `<` |
| F2 | `"` | `)` | `>` |
| F3 | `#` | `/` | `?` |
| F4 | `$` | `*` | `:` |
| F5 | `%` | `-` | `,` |
| F6 | `&` | `+` | `;` |
| Space | `^` | Down | π |
| Up | √ | `=` | `@` |

The typer/keyboard-mapping layers (`PC1500BasicTyper::typeLine`,
`PC1500KeyboardMap`) implement this by tapping `SHIFT` immediately before
each shifted character and never touching it otherwise — no explicit
"clear the latch" tap between a shifted and unshifted character, since
the latch is already consumed by whichever key follows it.

## Open

`STRING_VARS` (an address range sometimes cited for fixed string
variables elsewhere) was not confirmed by this investigation and isn't
needed now that `A$`–`Z$`'s real location is confirmed at `0x78C0`.
