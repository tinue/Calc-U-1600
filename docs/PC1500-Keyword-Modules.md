# PC-1500 keyword modules

How a ROM on the PC-1500's expansion bus adds BASIC commands that work like
built-in ones: typed, abbreviated, stored as tokens in programs, with the
usual error messages. The CE-150 and CE-158 add their commands this way, and
so does the RENUM example of the debugger's *PC-1500 ROM extension* template
([Debugger, §3](Debugger.md#example-renum-on-the-pc-1500)).

Everything here is read from the PC-1500 system ROM, except where noted.
The addresses are the same in revisions A01, A03 and A04, and the RENUM
template was tested against it in Calc-U-1600. Routine names follow the
ROM disassembly credited in the
[acknowledgments](../THIRD-PARTY-NOTICES.md#acknowledgments).

## Where the firmware looks

The firmware looks at eight 2 KB pages, &8000, &8800, … &B800. It looks at
each one twice, with PV low and with PV high, so there are 16 places a
module can sit. A page holds a module when its first byte is `55H`.

The search happens in three places:

| When | What happens | ROM |
|---|---|---|
| Power-on | for each module, the code at page + `0AH` is called | `TOK_TABL_SRCH` E4A8, `RESET_19` E10B |
| A word BASIC doesn't know is typed | each module's index and table are searched for the name | `TOK_INBUF_14` F9C7 |
| A module token is executed | the token's own page is searched for the code | `TOK_PROCESS` FA89 |

Calc-U-1600 plugs such a ROM in with `bus-rom:` in a preset, with `pv:` for
the PV state it answers in (see the [User Guide](User-Guide.md)).

## The page

| Offset | Contents |
|---|---|
| +00 | `55H` |
| +0A | power-on code, ending in `RTN`; a bare `RTN` if there is nothing to do |
| +20 | the index: 26 words (high byte first), one per letter A–Z |
| +54 | the keyword table |

**The index.** Each word is the address of the *second* letter of the first
keyword with that initial, or 0 if no keyword starts with it. A word that
isn't in the index can't be found.

**The table.** One entry per keyword, ended by a byte whose low nibble is 0:

```
marker  name   code (2 bytes)  address (2 bytes)
 C5     RENUM  E1 80           88 5F
```

- **marker:**
  - The low nibble is the name's length.
  - The high nibble belongs to the keyword *before* this entry, as in the ROM's own table at C054 (the disassembly's `CNIB` macro). Bit 4 set means that keyword was the last one with its initial: a search that fails on it stops there.
  - Bits 6–5 say where the keyword may run:

    | Bits 6–5 | The keyword runs |
    |---|---|
    | `00` | only typed at the prompt; inside a program it is ERROR 1 |
    | `01` | only inside a program (`FOR`, `READ`) |
    | `10`, `11` | both (`PRINT`, `GOTO`) |

    With `00`, the routine's first byte is a mask of the modes it may be typed in: `40H` RUN, `20H` PRO, `10H` RESERVE. The firmware ANDs it with 764FH (ERROR 26 if nothing is left), then starts the routine at the *next* byte. `LIST` (C96E) starts with `20H`, `RUN` (C8B4) with `40H`.
  - So the byte after the last entry of a table ends the table (low nibble 0) and also carries the last keyword's attributes. RENUM's is `90H`: last R keyword, typed only.
- **name:** ASCII, with no terminator. Typing a prefix with a period (`REN.`) finds it too.
- **code:** the token stored in the program, high byte first.
  - **The high byte names the page.** `E0H`–`E7H` are pages &8000–&B800 with PV low, and `E8H`–`EFH` the same pages with PV high: page = `80H + (code & 7) × 8` (`TOK_PROCESS` FAA8). A token runs only from the page its code names.
  - `F0xxH` codes are searched for on every page: these are the printer commands that the base ROM knows by name.
  - On page &8000 with PV low (`E0H`), only one keyword per initial can be found. When a name doesn't match, the search steps to the next entry by looking for a byte above `E0H` (F9F5–FA3E), and `E0H` itself isn't above it.
- **The low byte of the code** says what kind of keyword it is (checked by the expression evaluator):
  - `80H` and up: a statement;
  - `60H`–`7FH`: a function with an argument;
  - `50H`–`5FH`: a function without one.
- **address:** where the routine starts, high byte first.

## The routine

**Entry.** A statement's routine is entered with Y pointing just past its
own token, at the rest of the statement as BASIC stores it: spaces removed,
BASIC's keywords tokenized. Typed at the prompt that is the input buffer;
in a program it is the program line.

**Arguments.** The routine parses its own arguments with the ROM's vector
calls:

| Call | Does |
|---|---|
| `VEJ DE` + offset | evaluate the expression at Y; on an error, branch (UH = error) |
| `VEJ D0` + range + offset | convert the result to an integer in U (`00`: 16 bits) |
| `VEJ C2` + char + offset | the next element must be that character, else branch |
| `VEJ C8` + offset | branch unless the statement ends here |
| `VEJ E0` / `VEJ E4` | raise ERROR UH / ERROR 1 |

Branch offsets after a `VEJ` are forward, counted from the byte after the
offset (in sdas: `.db TARGET-.-1`). sdaslh5801 writes the vector as a plain
number: `vej 0xDE`, not `vej (DE)`. A routine that changes the program
calls `INIT_SYS_ADDR` (CFD0) as the ROM's own line editor does. That resets
the DATA pointer, the GOSUB/FOR stacks, ON ERROR and CONT.

**Exit.** A statement ends with `VEJ E2`, with Y at the end of its statement
(`:` or `0DH`). That continues the program, or completes a typed command.
It works for both, so no other clean-up is needed.

**Functions** (low byte below `80H`; the evaluator sorts the tokens at
D8A3–D8CF) are called from the middle of an expression instead. This part
isn't used by RENUM and isn't checked against the ROM
([acknowledgments](../THIRD-PARTY-NOTICES.md#acknowledgments)): the
argument is already evaluated, in the arithmetic register at 7A00H. The
routine leaves its result there and returns with `RTN` and UH = 0; a
non-zero UH is raised as that error.

**Errors.** To report an error as one in a program line ("ERROR 11 IN 30",
and the up arrow shows the line), RENUM sets the current line 789CH and the
program start 789EH before `VEJ E0`. It calls `INIT_SYS_ADDR` first, so that
an `ON ERROR GOTO` of the last run doesn't catch the error.

## The program in memory

RENUM changes the program, so it relies on its layout:

- The program runs from BASPRG_ST (7865H) to BASPRG_END (7867H), where its
  end byte `FFH` is. After a MERGE, BASPRG_EDT (7869H) points at the last
  merged program.
- A line is: number (2 bytes, high first), length (the bytes that follow,
  up to and including `0DH`), the tokenized text, `0DH`.
- Line numbers after `GOTO`, `GOSUB`, `THEN` and `RESTORE` are decimal
  digits, not binary as on the PC-1600: `GOTO 30` is `F1 92 33 30`. A
  reference that gets more digits makes the line longer.
- The ROM's line editor (`PRGLINE_TDI` CF27) checks that the new end stays
  below RAM_END_H (7864H), or it raises ERROR 13. Then `DEL_DIM_VAR_1`
  (D09C) drops the arrays if the program has grown into them.
