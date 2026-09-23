# Memory Card Definition File — Concrete Format

This is the serialization pass deferred by
`Memory-Card-Definition-Spec.md` §10. That document is the authority on
*what* a definition expresses and *why*; this one only pins down *how* it
is written. Every construct below maps to a numbered part of the spec —
the mapping is called out inline as (spec §N).

## 0. Format choice

- **YAML**, UTF-8, one card per file, extension `.card.yaml`.
- Chosen over JSON/plist because a definition is hand-authored and
  review-heavy: it needs comments (especially around ROM byte blocks and
  the reasoning behind a dormant sub-region), and it sits next to the
  project's existing YAML scenario files (`examples/*.pc1600`).
- Integers may be written decimal (`8192`) or hex (`0x2000`); hex is
  conventional for addresses and byte values.
- `#` starts a comment anywhere.
- Unknown keys are an error, not ignored — a misspelled field must not
  load silently.

The standard cards are bundled with the app under `Calc-U-1600/Resources/`
(new definitions go straight there); the CLI points `--modules-dir` at the
same directory.

---

## 1. Top-level keys (spec §2)

| Key | Required | Value |
|---|---|---|
| `module-name` | yes | Short identifier for the module (e.g. `CE-155`, `CE-1600M`). Shown verbatim in the GUI's control-bar module picker, and the key a preset's `modulespec: <module-name>` resolves against (§8). Unique across the module directory. |
| `compatible-hosts` | yes | List, non-empty, drawn from `PC-1500`, `PC-1500A`, `PC-1600-Slot-1`, `PC-1600-Slot-2`. The loader's **only** compatibility check (spec §1): a host not on this list is declined; a host on it gets no further test. |
| `definition-terminology` | yes | Exactly one of the same four identifiers. Names the pin-name vocabulary every `chip-select` / `signal` / `line` field in the file is resolved against, once, at parse time (spec §1). Must appear in `compatible-hosts`. |
| `regions` | yes | List of one or more region maps (§2). |
| `battery` | no | `true` for battery-backed hardware. A battery-backed *template* offers the app's Name & Save; ignored by the loader otherwise. |
| `template` | no | `true` marks a read-only **template** (§8): the app never writes to the file, wherever it is stored. Absent or `false` = an instance, autosaved in place. The app never writes this key into a saved instance. |
| `notes` | no | Free text; ignored by the loader. |

The reset-on-load behaviour (spec §2, last bullet) is a loader invariant,
not a field — no key can opt out of it.

### Terminology → signal names

The name in a `chip-select` / `signal` / `line` field must be legal for
the declared `definition-terminology`:

| Terminology | Legal signal names (besides `A0`… address / `D0`… data lines) |
|---|---|
| `PC-1500` | `Y0`, `Y2`, `S1`, `S2`, `S3`, `S4`, `PU`, `PV` |
| `PC-1500A` | `Y0`, `Y2`, `S3`, `S4`, `S5`, `PU`, `PV` |
| `PC-1600-Slot-1` | `RAM2`, `PVOUT`, `S1`, `S2`, `S3`, `PU`, `PT` |
| `PC-1600-Slot-2` | `RAM1`, `PVOUT`, `K0`, `K1`, `K2`, `PU`, `PT` |

This table is the source-of-truth pin dictionary from
`Core/Connector/ExpansionCard.hpp` (the `pin[]` role comment): each name
resolves, once at parse time, to one physical 40-pin contact
(`Y0`/`RAM2`/`RAM1` → pin 4, `S4`/`PVOUT` → pin 5, `S1`/`S3`/`K0` → pin
16, `S2`/`S4`/`K1` → pin 17, `S3`/`S5`/`K2` → pin 18, `Y2`/`PT` → pin 19,
`PU` → pin 3, `PV` → pin 2).

Whether a name resolves to a live pin or a dormant one on a *particular*
compatible host is not checked here — that is the author's dormant
sub-region judgement (spec §1).

---

## 2. Region map (spec §3)

| Key | Required | Value |
|---|---|---|
| `name` | yes | Identifier, unique within the file. |
| `addressing` | yes | Addressing spec (§3) — dimension D1. |
| `content` | yes | Content spec (§4) — dimension D2. |
| `banking` | yes | `none`, or a banking map (§5) — dimensions D3/D4. |
| `capacity` | unbanked only | Backing-store size in bytes for the one copy of the region. Required when `banking: none`; omitted when banked (there it is `bank-count * bank-size`). |
| `initial-content` | ROM: yes (must cover every byte) · Flash/Regular: optional | Byte content carried in the file (§6) — spec §5a. Allowed for any content kind, no restriction — a Regular (RAM) range can power up pre-loaded exactly like a Flash range's initial image. |
| `pc1600-module-class` | PC-1600 regions only | `program` \| `system` \| `ram-disk` \| `plain-ram-no-header` — authoring cross-check against the header bytes in `initial-content` (spec §7). Omit on PC-1500/1500A regions. |

---

## 3. Addressing spec (spec §4)

An addressing spec is **enable groups OR'd together**; each group is
**terms AND'd together**. It appears in three places, all the same shape:
a region's `addressing`, a banked region's `bank-window`, and (in
principle) deeper nestings.

```yaml
addressing:
  any-of:                      # OR across groups
    - all-of:                  # AND within a group
        - chip-select: Y0
        - address-bits: { A13: 1, A12: 1, A11: 1 }   # the on-module decoder's select inputs
      span: 0x0800             # 2 KB — served via the low 11 address lines (A0–A10)
      maps-to: 0x0000          # where this slice lands in the backing store
    - chip-select: S1          # bare dedicated strobe, no address decode on this path
      span: 0x0800
      maps-to: 0x0800
    - chip-select: S2
      span: 0x0800
      maps-to: 0x1000
```

Shorthand: a single-group spec may drop `any-of` and give `all-of`
directly; a single-term group may drop `all-of` and give the bare term.

### Group terms

| Term | Meaning (spec §4) |
|---|---|
| `chip-select: NAME` | Named line asserted. Friendly alias for `signal`. The common case: one dedicated strobe standing for one slice of storage, with no address decode on that path. Several such strobes for one region are separate OR'd groups (one per `- ` entry under `any-of`), **not** a list. |
| `chip-select: [A, B, C]` | All listed lines asserted at the same access (an AND). Uncommon — the host's 2 KB strobes are mutually exclusive, so this is only for a strobe that is qualified by another live signal. |
| `signal: NAME` / `signal: [..]` | Same as `chip-select`; use when the line isn't a decode strobe (e.g. `PVOUT`). |
| `signal-negated: NAME` | Line explicitly **not** asserted. Needed to split a window by a half-select line (`PVOUT` low vs. high). |
| `address-bits: { An: 0\|1, … }` | Specific address lines required at specific levels, AND'd into the group. Models an on-module decoder that gates on a **few** address lines alongside a chip select — *not* a full address-bus comparison. CE-155's `Y0` chip: `{ A13: 1, A12: 1, A11: 1 }` (the "`AD11–AD13 = 111`" of `PC-1500-Address-Decoding.md` §3.2). Lines not listed are not tested. Does **not** set `span` — the low, un-listed lines are the offset. |
| `memory-range: { from, to }` | A raw address window the card magnitude-decodes itself, with **no** chip select in the group (spec §4 direct memory-range match). Fixes `span` from `to − from + 1`. Flagged distinctly because the author's host-compatibility judgement has no pin to anchor to (spec §1). |

### Group keys

| Key | Default | Meaning |
|---|---|---|
| `span` | required unless the group has a `memory-range` | Size in bytes of the storage slice behind this group. The module serves it using the low `log2(span)` address lines as the offset (`0x0800` ⇒ A0–A10); it does **not** decode any higher line except those an `address-bits` term names. |
| `maps-to` | groups packed in declaration order, each starting where the previous ended | Byte offset in the region's backing store (or, inside `bank-window`, the current bank's slice) that the low end of this group's slice maps to. |

The groups' slices must tile `capacity` (unbanked) or `bank-size`
(banked) exactly — no gap, no overlap.

---

## 4. Content spec (spec §5)

### Whole-region, one kind

```yaml
content: rom            # bare string form: regular | rom | flash (defaults for each kind)
```

```yaml
content:
  kind: regular
  writable: true              # false = wired read-only (not a togglable switch)
  power-up-fill: 0x00         # per-range power-up byte; default 0x00 (CMOS RAM after a power loss, spec §8)
  write-protect:              # optional runtime-togglable protect (CE-159); omit if absent
    default: unprotected      # unprotected | protected — state on load (spec §8)
    persisted: false          # false = always resets to `default` on load; true = part of card state
```

```yaml
content:
  kind: flash
  power-up-fill: 0xFF         # default 0xFF (erased); applies only where `initial-content` leaves gaps
  protocol:                   # chip/firmware properties, not inferred (spec §5)
    unlock-sequence:
      - { address: 0x5555, data: 0xAA }
      - { address: 0x2AAA, data: 0x55 }
    byte-program-command: 0xA0
    erase-setup-command:  0x80
    sector-erase-command: 0x30
    chip-erase-command:   0x10
    sector-size: 0x1000
    reset-command: 0xF0
    command-address-mask: 0x7FF   # optional; default = full address, no masking.
                                   # How many low address bits the command
                                   # decoder actually looks at before comparing
                                   # against an unlock-sequence/command address --
                                   # some flash chips (recovered from a real
                                   # CE-163F firmware disassembly) decode fewer
                                   # bits than the full bus, so an unlock write's
                                   # upper address bits don't have to match.
```

Every field above is required except `command-address-mask` — there is no
sensible default for a chip/firmware property (spec §5), so `content: flash`
as a bare string (no `protocol:`) is a load error, not a "use the defaults"
shorthand.

`rom` takes no options beyond the implicit read-only-forever rule; its
bytes come from `initial-content` (§6), which is mandatory for ROM.

### Split content in a banked region (spec §5 split note)

```yaml
content:
  by-bank:
    - { banks: "0-7",  kind: regular, power-up-fill: 0x00 }
    - { banks: "8-15", kind: flash, protocol: { ... } }
```

`banks` is a `"lo-hi"` string or a single integer. Ranges must partition
`0 .. bank-count-1` with no gap or overlap. A single-kind banked region
just uses the whole-region form above.

---

## 5. Banking map (spec §6)

```yaml
banking:
  latch:
    type: trigger-based            # trigger-based | line-based
    # --- trigger-based ---
    trigger: { pin: 18 }           # a memory-write strobe pin (CE-163: S3),
                                   # OR { io-port: 0x28 } for an OUT (n) write (CE-1601M)
    sampled-lines: [A0, A1, A2, A3]  # bits latched on the trigger; sampled-lines[0] is the LSB
    source-domain: address         # address → bits of the address bus (CE-163)
                                   # data    → bits of the written byte (CE-1601M's OUT (28H))
    # --- line-based (instead of trigger/sampled-lines) ---
    # lines: [K0, K1, K2]          # the bank number read live off these lines
    # source-domain: address-select
  bank-count: 2                    # live banks (may be < what the latch width allows)
  bank-size: 0x8000               # bytes per bank
  bank-window:                     # addressing spec (§3), nested one level (spec §4/§6)
    any-of: [ ... ]
```

A banked region's top-level `addressing` (§2) is a **pure gate** — "is
this access one my banking applies to at all" — so it carries only
terms, no `span` / `maps-to`. All slice mapping happens in `bank-window`,
which carries its own full addressing spec so a second, independent
sub-range (CE-1601M / *superRAM* `PVOUT`) can be expressed as
"selected-bank AND half-select line", one level deeper (spec §4). Inside
`bank-window`, `span` / `maps-to` offsets are into the current bank's
`bank-size` slice.

Both confirmed banked modules are **trigger-based** and differ only in
`source-domain`: CE-163's `{ pin: 18 }` strobe samples the **address**
bus; CE-1601M's `{ io-port: 0x28 }` write samples the **data** bus. Both
latch the decoded bank internally until the next trigger.

Descriptive "Vertical / Horizontal" is **not** a field (spec §3):
line-based = Horizontal, trigger-based = Vertical.

---

## 6. `initial-content` (spec §5a)

Bytes fixed by the definition — all of ROM's content (required), or a
Flash or Regular (RAM) range's *initial* array (optional; absent ⇒ the
range powers up as its `power-up-fill` byte, same as today). There is no
content-kind restriction: a Regular range takes `initial-content` exactly
like a Flash range does — CE-163F's RAM banks and Flash banks are both
populated this way when dumped and pasted back in (§9). Never an external
runtime file.

```yaml
initial-content:
  fill: 0xFF                 # bytes no block covers; default is the bank's
                              # own power-up-fill (content §4), not always 0xFF
  blocks:
    - bank: 0                # omit entirely for an unbanked region
      offset: 0x0000         # start offset within the region / bank slice
      encoding: hex          # hex | base64 | addressed-hex | file
      bytes: |
        43 16 00 00 ...       # whitespace/newlines ignored for hex
    - bank: 1
      offset: 0x0000
      encoding: file
      path: ce1620m-bank1.bin   # sidecar, resolved relative to this file and
                                # folded inline at parse time (no run-time lookup)
```

- **Multi-bank** content is just multiple `blocks` with different `bank:`
  keys — one file populates every bank (spec §5a). Two blocks touching
  the same bytes is an error.
- A region/bank-range marked `rom` with no covering block is incomplete
  and is rejected at load (spec §5a). A `flash` range with none is valid.
- For a PC-1600 header-bearing module the 8-byte header and jump table
  are just bytes in a block at their fixed offset (spec §7) — there are
  no separate header fields.

### `encoding: addressed-hex`

A project-specific dialect designed so a memory dump can be pasted
straight into a `bytes: |` block with no reformatting — the "dump whole
card, ready to paste" debug feature (§9) emits exactly this. Lines are
either an explicit row or a same-byte run:

```yaml
bytes: |
  $0000: 00...
  $0050: 00 00 00 00 00 00 11 F1  8A 26 45 33 2C 58 40 12
  $0060: F0 97 22 42 41 4E 4B 20  22 3B F1 6F 26 45 32 40
  $0070: 13 F0 97 22 56 32 30 22  40 FF FF FF FF FF FF FF
  $0080: FF...
  $3FF0: AA...
```

- `$XXXX: <up to 16 space-separated hex byte pairs>` places those literal
  bytes starting at that address.
- `$XXXX: XX...` (the only token on its line) means "byte `XX` repeats from this address up to (but not
  including) the next line's address" — or up to the end of the block's
  span for the last line. This is exact, not a guess: because lines must
  partition the block's address range with no gap or overlap, a run's
  length is always computable from the surrounding addresses, unlike the
  existing elided `debugDumpMemRegion` console dump (which drops a run
  with no record of which byte value it was).
- A run only ever stands for one uniform byte value — content that
  alternates between two fill values (e.g. `00`/`FF` rows) is written out
  in full, the same as an ordinary row.

### Implemented encodings

`addressed-hex`, `hex`, and `base64` are implemented
(`Core/Connector/MemoryCardDefinition.hpp`). `encoding: file` is parsed
but not yet supported — it would need the definition file's own directory
threaded through the loader, which nothing else currently needs.

---

## 7. Worked examples

| File | Card | Shows |
|---|---|---|
| `Calc-U-1600/Resources/ce155.card.yaml` | CE-155, 8 KB | one `address-bits` group (the on-module `AD11–AD13` decoder) OR'd with three bare-strobe `span` groups; an author-declared host list (spec §1) covering PC-1500 and PC-1600 Slot 1. |
| `Calc-U-1600/Resources/ce1600m.card.yaml` | CE-1600M, 32 KB | `PVOUT` folded into addressing as a half-select line — unbanked despite two physical halves (spec §6); one Slot-1-terminology file declared for both PC-1600 slots (`RAM2` resolves to the pin-4 enable on both — spec §1). |
| `Calc-U-1600/Resources/ce1601m.card.yaml` | CE-1601M, 64 KB | Trigger-based vertical banking — `trigger: { io-port: 0x28 }`, `source-domain: data` (the byte written by `OUT (28H)`); `bank-window` nesting the `PVOUT` half-select (spec §4/§9). |
| `Calc-U-1600/Resources/superram.card.yaml` | superRAM, 512 KB | The CE-1601M mechanism with a 4-bit (`D0`–`D3`) `OUT (28H)` latch and all 16 × 32 KB vertical banks fitted; PC-1600 Slot 2 only (spec §6/§9's "maxed-out Slot 2" row). |
| `Calc-U-1600/Resources/ce1638.card.yaml` | CE-1638, 128 KB | A single-kind banked region — trigger-based, address-domain, 8 banks sampling `A0`-`A2`; the real module `CE1638PlusCard.hpp`'s throwaway "+" test card is loosely modeled on. |
| `Qt6/resources/cards/ce502b.card.yaml` | CE-502B, 16 KB ROM | `content: rom` with one `addressed-hex` block covering the whole region: a Sharp program module (ROM header at &0000, BASIC program from &00C5) on Y0. |
| `Calc-U-1600/Resources/ce163f.card.yaml` | CE-163F, 256 KB | `content: by-bank:` (§4) splitting one 16-bank region into Regular (0-7) and Flash (8-15) content, and a `flash` `protocol:` block including `command-address-mask` for its real low-11-bit command decode quirk. |

---

## 8. Loading a card — `modulespec:` and `modulespecfile:` preset keys

A definition is plugged in from a preset scenario file, in the same
`memory-expansion*` block that takes a built-in `- module:` name — one
item, exactly one of the three keys, never combined:

```yaml
# By module-name, from the bundled/standard module directory (preferred).
# PC-1500 / PC-1500A
memory-expansion:
  - modulespec: CE-155

# PC-1600 (per slot)
memory-expansion-1:
  - modulespec: CE-1600M
memory-expansion-2:
  - modulespec: CE-1601M
```

```yaml
# By path to a definition file, relative to the preset's own directory
# (same rule as `program.path`). For a one-off card not in the directory.
memory-expansion:
  - modulespecfile: ../cards/prototype.card.yaml
```

- `modulespec: <module-name>` is resolved by scanning one or more **module
  directories** (`Core/Connector/MemoryCardCatalog.hpp`), in order, for the
  `.card.yaml` whose `module-name:` matches. WHERE those directories are is
  environment-specific, like the ROM path:
  - the **app** searches its bundled resources first, then its iCloud-Drive
    `BatteryCards/` folder — so a preset can name a user's own saved
    battery-card instance (matched by the `module-name:` written into it,
    e.g. `- modulespec: CE-1601M - Programs`; the on-disk filename, blanks
    and all, is not consulted);
  - the **CLI** uses `--modules-dir` (default `Calc-U-1600/Resources`),
    which may be repeated to add fallback directories.

  The first directory with exactly one match wins (so a bundled card
  shadows an instance of the same name). No match in any directory is a
  load error; two files claiming one name **within a single directory** is
  an ambiguity error. A missing/unreadable fallback directory is skipped
  silently.
- `modulespecfile: <path>` names a file directly and never consults the
  module directory.

**Templates and instances.** What happens to a loaded card's file is decided
by the file itself, not by where it lives:

- `template: true` — a **template**. The app never writes to it. Every
  bundled card is one, and a user can drop their own into the storage folder,
  where it is listed with the bundled cards. A battery-backed template
  (`battery: true`) offers **Name & Save**, which copies it (with the live
  contents as `initial-content:`) into `<name>.card.yaml` in the storage
  folder, without the `template` key.
- No `template` key — an **instance**. Any write to the card is autosaved back
  into that same file, wherever it was loaded from (never into the bundle).
  A preset's `saveas:` can copy an instance under a new name; the copy keeps
  the original template's name in its generated header comment.

Either way the loader picks the target host from the preset (`PC-1500` /
`PC-1500A`, or the slot number for `PC-1600`), checks it is in
`compatible-hosts` (spec §1/§2) — declining with an error if not — and
builds one `SoftwareDefinedCard` (`Core/Connector/SoftwareDefinedCard.hpp`)
that decodes purely from connector pins, exactly like a hand-written card.

The GUI's control-bar module picker uses the same catalogue: it lists
every bundled module whose `compatible-hosts` covers the open model /
slot, by `module-name`, and attaches the chosen one the same way.

## 9. Implemented subset (first iteration)

The parser reads the whole format and validates it, but the runtime card
currently supports:

- **Content:** Regular, Flash and ROM, single-kind or `by-bank`-split
  across a banked region's banks (spec §5's split-content note). A `rom`
  range ignores every write — guest CPU, host poke and the debug/loader
  backing-store path alike — and its `initial-content` must cover every
  byte (`fill:` doesn't count). `initial-content` is supported via
  `encoding: addressed-hex | hex | base64` (§6); `encoding: file` is
  parsed but rejected ("not supported yet").
- **Banking:** Unbanked, and **trigger-based** Banked (both a memory-write
  strobe `{ pin: N }` sampling the address bus and an `{ io-port: 0xNN }`
  write sampling the data bus). `line-based` is a hard load error.
- On the PC-1600, `pc1600-module-class: plain-ram-no-header` (or the field
  absent) needs nothing further; the header-bearing classes need an
  `initial-content` block covering the header/jump-table offset.
