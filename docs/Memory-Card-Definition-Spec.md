# Memory Card Definition File — Specification

**Status: content model.** This document defines *what* a memory card
definition file must express and *why*, in prose. The concrete
serialization — YAML key names, grammar, byte-block encoding — now lives
in `Memory-Card-Definition-Format.md`, with worked examples under
`examples/memory-cards/`; that spelling is revisable, this content model
is the fixed part. For a gentler, example-first introduction (starting
from the "fully maxed out" fictional PC-1500 card in
`examples/memory-cards/pc1500-maxed-out.card.yaml`), see
`docs/User-Guide.md`'s "Custom YAML memory cards" section — this document
and the format spec are the full reference the walkthrough links back to.
This document also isn't the "concrete
software-defined memory module" implementation itself
(`Core/Connector/SoftwareDefinedCard.hpp`, the one general-purpose runtime
module that *reads* files in this format) — it's the contract that
implementation and the file format both have to satisfy.

Every field this spec calls for exists because some documented mechanism
of the PC-1500/PC-1600 expansion-connector or bank-switching hardware
needs it to be expressible — §9 cross-checks that row by row against the
Technical Reference Manual and this project's own ROM traces.

---

## 1. Design principle: the loader connects pins; terminology is an authoring convenience with no other meaning

**The loader operates on physical pin numbers, full stop.** Every signal
a region's addressing/latch fields reference is, underneath, one of the
40 physical pins on the expansion connector — the loader's actual job at
run time is entirely expressible in terms of "pin N asserted," never in
terms of a name like `S1` or `Y0`. A definition file could be written
directly in pin numbers and nothing about how the loader behaves would
change; the file format would just be less pleasant to read and write.

**Signal-name terminology exists purely so the file's author can think
and write in signals instead of bare pin numbers — nothing more.** The
connector reuses the same physical pins for different signals across
PC-1500 vs PC-1500A vs PC-1600 Slot 1 vs PC-1600 Slot 2, so "S1" only
resolves to a pin once you say whose vocabulary it's drawn from; that's
why a file declares **one terminology** and every signal name in it is
looked up against that one table, once, at load/parse time, into a plain
pin number. After that lookup, terminology has done its entire job — it
carries no further semantic weight, is not consulted again, and has no
bearing on anything the loader decides afterward (including
compatibility, below). Supporting more than one terminology is a
convenience for authors who think in a specific host's own signal names
(most naturally, whichever host the real module they're modeling shipped
for); the file format would be fully expressible with a single, pin-
numbered terminology instead, at the cost of being harder for a person
to write and review.

**Every host's expansion connector is physically the same 40-pin port**
(`Software-Defined-Memory-Extension.md` §4) — a card built for one model
physically fits the slot of every other model listed here. Nothing about
the file format needs to model a plug that won't fit; the load-time gate
is not a connector/electrical simulation, it's a much simpler thing.

**Compatibility is an author-declared list, and the loader's job is
exactly one check: is the host being loaded into on that list?** If yes,
load; if no, decline. The loader does not independently derive, validate,
or second-guess that list against pin tables, NC pins, or per-host signal
roles — that judgment belongs to whoever authors the definition file, the
same way it belonged to whoever wired a real hardware module and decided
which hosts to sell it for.

A pin the file references that happens to be NC on one of its declared-
compatible hosts is not an error and not a reason to decline loading — it
is simply a sub-region of the card that never asserts on that host, the
same way a real hardware module would behave if plugged into a slot
missing one of the traces it expects. Two concrete cases:

- A card built to max out the PC-1500A (`Y0` + `S3`+`S4`+`S5`, i.e. pins
  4/16/17/18, never referencing pin 5) can freely declare PC-1500
  compatible too — on a PC-1500, those same four pins are `Y0`/`S1`/`S2`/
  `S3`, giving the identical 22KB, fully live; nothing is dormant in this
  particular case at all.
- A card built to max out the PC-1500 (adding pin 5 / `S4` to reach
  24KB) can just as freely declare PC-1500A compatible — the pin-5-driven
  2KB slice simply stays dormant there (pin 5 is NC on PC-1500A), while
  the rest of the card behaves normally, giving the same 22KB the
  PC-1500A-only variant would. This is a legitimate, author-approved
  product, not something the loader treats as broken.

An author who wants a card's behavior to be uniform (never dormant) on
every host it declares compatible can still choose to write it that way
— by simply never referencing a pin that's NC anywhere on that list — but
nothing about the file format or the loader forces that choice; a
partially-dormant card on some of its declared hosts is equally valid.

This also settles what "the module never knows which host it's plugged
into" means in practice: the loaded region's addressing/latch
configuration is fixed at authoring time and never branches on detected
host identity — but that's a property of a single file's own logic, not
a reason it needs a separate file per host. One file, with one fixed
addressing spec and one compatible-host list, is the whole story; a
dormant NC-backed sub-region on one of its declared hosts is an expected,
acceptable outcome of that one file, not evidence it should be split.

Where the author's judgment call matters most is a region using **direct
memory-range match** (§4), which decodes a raw address window with no
host-provided chip-select at all: the same numeric address range can mean
something different (or already be claimed by other hardware) on
different hosts, with no pin on the card to point to either way. The
mechanism for declaring compatibility is identical to the chip-select
case above — the author lists the hosts, the loader checks the host
being loaded is on that list — but getting the list right for this kind
of region depends entirely on the author's own knowledge of each host's
address map, since there's no signal name to anchor the decision to.

---

## 2. Required top-level identity

- **Module name** — display name shown in the model-selection dropdown
  (e.g. "CE-155 8KB Memory Module", "16-bank CE-163-alike").
- **Compatible host list** — subset of `{PC-1500, PC-1500A, PC-1600 Slot 1,
  PC-1600 Slot 2}`, chosen by the author. This is the loader's sole
  compatibility check: it declines to load a card into a host that isn't
  on this list, and imposes no further test on a host that is (§1) — no
  pin-existence check, no signal-role check. A card is not restricted to
  the one host whose vocabulary wrote it; declaring several hosts
  compatible, including ones where part of the card ends up dormant
  because a referenced pin is NC there, is an ordinary, valid choice
  (§1).
- **Definition terminology** — exactly one of the same four host
  identifiers, naming which pin-name vocabulary every signal field in this
  file uses. Must appear in the compatible-host list (a file's own
  terminology host trivially belongs on its own compatible-host list),
  but the list is not limited to that one entry.
- **Load behavior on insert/change** — loading a card resets the
  calculator; not itself a per-file field, but worth stating here as an
  invariant the loader guarantees regardless of what the file declares
  (no card can opt out of the reset).
- **`battery`** *(optional bool, default `false`)* — marks the card as
  battery-backed real hardware (CE-163 and its derivatives, CE-1600M,
  CE-1601M, CE-1638). Has no effect on region/content parsing; it only
  gates the app's name-and-save persistence flow — a battery-backed card,
  once attached, can be named and saved as a standalone, independently
  persisted instance file (itself a complete `.card.yaml`, generated by
  splicing a live dump of the card's own backing store into the original
  template's source text — the splicing itself is a shared Core function,
  `spliceBatteryCardInstance()` in `Core/Connector/BatteryCardInstance.hpp`;
  the GUI-side orchestration — naming, save triggers, the instance
  directory — is `Qt6/app/MemoryModuleManager.cpp` + `AppPaths.cpp`)
  that keeps its contents durably up to date across reloads and app
  restarts. A card without `battery: true` stays purely volatile.

---

## 3. Dimension model

A card is a list of one or more **memory regions**. Every region is fully
described by four **independent dimensions**. Independent means: each is
specified once, in its own place, and none of the other three can be
inferred from it — a region's addressing doesn't tell you its content
type, its content type doesn't tell you whether it's banked, and so on.

| # | Dimension | Values | Specified |
|---|---|---|---|
| D1 | **Addressing** | chip-select set (with AND/OR combination) · address-line qualifier (specific lines at specific levels, not a bus-window compare) · direct memory-range match — see §4 | Once per region. If the region is banked, a *second* addressing spec of the same shape nests inside it as the bank window's own sub-range (needed for CE-1601M/*superRAM*'s PVOUT — see §6/§9) |
| D2 | **Content** | Regular (CPU-read/writable) · ROM (bytes carried inline in the definition file, never CPU-writable) · Flash (optional initial content carried inline in the definition file, CPU-rewritable at runtime via a command-sequence protocol) — see §5 | Once per region if unbanked. If banked, once **per bank-range** (not necessarily once per region) — see §5's split-bank note |
| D3 | **Banking presence** | Unbanked · Banked | Once per region — see §6 |
| D4 | **Bank latch mechanism** *(only if D3 = Banked)* | Trigger-based (a strobe — a pin, or an `OUT` to a port — whose assertion is the instant the module samples a bank-data source and latches it internally: CE-163 samples the address bus, CE-1601M the data bus) · Line-based (an always-visible signal group read live, no internal latch — expressible but unexercised) — plus which signal(s)/pin(s) and which bus domain | Once per region — see §6 |

There is deliberately **no separate "orientation" field.** Every value a
Vertical/Horizontal label could take is fully determined by which
signal(s) and which mechanism D4 already names — a trigger-based latch on
pin 18 sampling `A0` (CE-163) or on `OUT (28H)` sampling `D0`–`D2`
(CE-1601M) *is* what "Vertical" means; a line-based latch read live off
`K0`–`K2` *is* what "Horizontal" means. Asking the file to state both
would just be asking the same fact twice. **Vertical/Horizontal remain in
this document as descriptive terminology** — they're the words this
project's other docs and Sharp's own manuals use, and they're useful
shorthand when talking *about* a card's mechanism — but they are not
fields a definition file needs to fill in; the file only ever needs to
say Trigger-based or Line-based, plus the signal(s) and domain. §6 below
covers both confirmed banked families (both trigger-based, differing only
in the sampled bus), for readers translating between the two vocabularies.

Addressing (D1) is genuinely orthogonal to everything else and is
specified **once**, as a single reusable sub-model that other dimensions
reference rather than redefine — not a separate "enable condition" field
repeated once per content kind and once per banking kind. §4 below is
that single addressing sub-model; §5–6 all *use* it rather than defining
their own version of it.

---

## 4. D1 — Addressing (shared, specified once)

Every kind of memory needs to be addressed — this is common to all
regions regardless of where they sit on D2–D4, so it's specified exactly
once as a sub-model that other dimensions reference rather than repeat.

An addressing specification is one or more **enable groups**, OR'd
together; each group is one or more signals/qualifiers, AND'd together:

- **Chip-select signals** — named lines in the file's declared terminology
  (e.g. `Y0`, `S1`, `S2`, `S3`; PC-1600's `RAM1`/`RAM2`, `K0`–`K2`). Each
  named line resolves to a physical pin via the terminology (§1); whether
  a pin is wired on a given declared-compatible host or stays NC there is
  exactly the author's dormant-sub-region case from §1, not something
  checked at load time. A group **may** list more than one line, meaning
  all of them asserted at the same access (an AND) — but that is
  uncommon, because the host's own decoder makes its 2KB strobes mutually
  exclusive. The usual multi-strobe module instead wires to several such
  strobes and responds to *whichever* is asserted, each strobe standing
  for a different 2KB of its storage: that is an **OR of single-line
  groups** (below), not an AND. **CE-155's `S1`/`S2`/`S3` are exactly
  this** — three mainboard chip-selects, each hard-wired straight to one
  dedicated 2KB RAM chip on the module, with no on-module address decode
  on that path at all (the module never sees an address it decodes here;
  it just uses the low 11 bits as the offset into whichever 2KB the
  asserted strobe points at). Genuine AND-of-lines shows up instead when
  a single strobe is only valid alongside another live signal — e.g.
  PC-1600 `RAM1` together with a particular `PVOUT` level (§6/§9).
- **Address-line qualifier** — a handful of specific address lines
  required at specific levels, evaluated *together with* a chip select,
  standing for a real piece of on-module decode logic. It is **not** a
  magnitude comparison of the whole address bus against a `from`/`to`
  window. CE-155's `Y0` chip is the example: the module's own 138-family
  decoder tests `AD11 = AD12 = AD13 = 1` (its select inputs) while `Y0`
  holds its enable — three lines, nothing else. The lines *below* the
  ones it tests (`A0`–`A10` here) are the offset into the chip, not part
  of the decode; the lines it doesn't mention are simply not checked. The
  window this "works out to" (`&3800`–`&3FFF`) is a derived description,
  not what the hardware evaluates. A pure chip-select path with no
  on-module decode at all (CE-155's `S1`/`S2`/`S3`) carries no qualifier
  of either kind; its span is just the strobe's dedicated slice.
- **Direct memory-range match** — a plain address window with no chip
  select at all, for modules meant to decode a raw address window
  themselves rather than relying on the host's own named strobes (a
  region that listens to a memory range directly, not via chip select).
  Compatibility here is declared the same way as everywhere else (§2/§1),
  it just has no pin to anchor the author's judgment to.
- **OR across groups** — a region can be reachable more than one way
  (CE-155 again: `(Y0 AND sub-range)` **OR** `S1` **OR** `S2` **OR** `S3`
  — four groups, three of them a single bare strobe). An addressing spec
  is a list of such groups, not a single one.

This same sub-model is reused, unmodified, in two other places:
- A region's own top-level addressing (when unbanked, or as the "is this
  a bank-selecting access at all" condition when banked).
- The **bank window**'s addressing, nested one level inside a banked
  region (§6) — needed because CE-1601M/*superRAM* apply a second,
  independent line-level condition (the `PVOUT` half-select) *inside*
  whichever bank is currently selected, which is exactly "chip-select
  (the selected bank) AND a required line level (`PVOUT`)," the same
  shape as CE-155's `Y0`-path group above, just composed one level
  deeper (§9).

---

## 5. D2 — Content: Regular vs. ROM vs. Flash

- **Regular** — CPU-read/writable at runtime. Fields: a writability flag
  (read/write vs. wired read-only) and, separately, an optional
  runtime-togglable **write-protect** state (CE-159: identical addressing
  and capacity to CE-155, but with a switch the running system can flip —
  not the same thing as being wired read-only), plus its default on load
  (§8).
- **ROM** — bytes supplied by an explicit **byte-content section carried
  inside the definition file itself** (§5a), not by a separate
  preload/image file; never written by the running calculator, under any
  circumstance — read-only by definition, not by a togglable switch, and
  with no on-module mechanism that could ever make it otherwise. This is
  CE-160's mechanism ("custom invisible BASIC programs" via
  dealer-programmed write-protected RAM-as-ROM — electrically it's
  protected RAM, but functionally fixed at authoring time and never
  rewritten again by anything, so it's modeled as ROM rather than as
  permanently-protected Regular) and CE-1620M's.
- **Flash** — may also carry an *initial* array, and when it does, those
  bytes are specified the same way ROM's are: in the definition file's own
  byte-content section (§5a), never an external file. Flash's initial
  array is optional (a Flash range with none declared powers up erased,
  all `0xFF`); the difference from ROM is only what happens next — unlike
  ROM, the real chip accepts CPU-driven rewrites at runtime. The
  distinguishing test against both other kinds is *how*
  those rewrites happen — never a plain memory-mapped store the way
  Regular is, and never impossible the way ROM is:
  - An ordinary CPU write into a Flash range does **nothing** to the array
    by default. The module's own command decoder — a small state machine
    sitting behind whatever D1 addressing/D4 bank-latch selects the
    range, and independent of both (see below) — must first see a
    specific address/data write sequence (a JEDEC-style unlock cycle) before
    a write is armed to actually take effect.
  - Two operations exist once armed: **byte-program**, which can only
    *clear* bits (`array[addr] &= data`, true NOR-flash semantics — a
    program alone can never set a bit back to 1), and **erase**, which
    resets a range of bytes to `0xFF`, the only way to undo a program.
    Erase granularity is chip-defined — a **sector erase** clears one
    fixed-size block, a **chip erase** clears the whole Flash content-range
    at once. Any write that doesn't match the sequence's next expected
    step resets the decoder back to idle (a glitch-recovery property, not
    an error condition to reject at authoring time), and a fixed reset
    command returns an already-armed decoder to plain read mode without
    completing an operation.
  - A definition file needs enough fields to state this protocol
    concretely for a given Flash content-range: the command address(es)
    and the data byte(s) each must carry at each step of the unlock
    sequence, the data byte(s)/command that arm byte-program vs.
    sector-erase vs. chip-erase, the sector size (for sector-erase), and
    the reset command. These are chip/firmware properties, not something
    the loader infers — the exact values in current use are recorded next
    to the module's own reference material rather than duplicated
    verbatim here.
  - **The command decoder is its own state machine, independent of D3/D4's
    bank-select mechanism**, even when the same physical control lines
    happen to carry both a bank-latch strobe and part of an unlock
    sequence in quick succession on real hardware. A bank-select write
    must never reset or otherwise perturb the command decoder's state —
    real firmware routinely interleaves the two (writing the module's
    bank/window-select port between the unlock cycle's own address/data
    writes), and a definition file/loader that conflated the two state
    machines would break that interleaving, wedging the real firmware's
    post-operation verify/poll loop indefinitely.
  - Confirmed example: **CE-163F**, a modern (non-Sharp-original) 16-bank
    PC-1500/1500A module — banks 0–7 Regular RAM, banks 8–F Flash
    (Microchip SST39SF010A: bank 8 reserved for the module's own
    management firmware, banks 9–F user-writable), all 16 banks selected
    by the same trigger-based latch on pin 18 sampling `A0`–`A3` (§6). A
    hardcoded connector-layer prototype (`Core/Connector/CE163FCard.hpp`)
    implemented this ahead of the general-purpose software-defined module
    this spec describes — that implementation (unlock addresses,
    program/erase state machine, the bank-latch/command-decoder
    independence above) was the concrete reference for every Flash field
    named above, recovered from a real CE163F firmware disassembly, and
    its engine has since been ported into `SoftwareDefinedCard` proper
    (see `Calc-U-1600/Resources/ce163f.card.yaml`, §9). Deliberately not
    modeled in either implementation, and so not required of a definition
    file: status-polling toggle bits (DQ6/DQ7) and software-ID/autoselect,
    both unused by that firmware; program/erase complete instantaneously
    rather than taking simulated time.

ROM and Flash share the same D1 addressing sub-model as Regular — content
kind is purely a question of where the bytes come from and whether/how the
CPU can change them, never a question of how the region is addressed.

**Split content within one banked region.** D2 is specified **per
bank-range**, not necessarily once for the whole region, because a real
banking scheme can mix kinds behind the same bank-select mechanism — CE-163F
above is exactly this: 8 Regular banks followed by 8 Flash banks, all
selected the same way. The definition file records this as a list of
`{bank range → content kind}` entries (e.g. `banks 0–7: Regular`, `banks
8–15: Flash`) rather than one kind per region — a single-kind region is
just the degenerate case of one entry covering every bank (or, if D3 =
Unbanked, the only entry, implicitly covering the region's one and only
"bank"). A ROM/Flash mix behind one bank-select, or any other combination
of the three kinds, is equally expressible even though no module in §9
happens to need it today.

### 5a. Where ROM, Flash, and Regular bytes are specified

Any content this spec pins down at authoring time lives **inside the
definition file itself**, never in an external preload/image file. That
covers all of ROM's bytes (mandatory — ROM is nothing without them) and,
when one is declared at all (optional, §5), a Flash range's *initial*
array or a Regular (RAM) range's own pre-loaded content — there is no
content-kind restriction on carrying one. All three are expressed through
the same **byte-content section**, which every ROM region must (it is
incomplete without one) and any other region or bank-range may carry
regardless of its D2 kind. It expresses:

- **The bytes themselves**, as an ordered sequence over the range's
  capacity. The concrete encoding (hex text, base64 blob, numeric array,
  or a parse-time include of a sidecar data file folded into the file) is
  a serialization question settled in Format.md §6 (`hex` / `base64` /
  `addressed-hex` / `file`) — the content-model requirement is only that
  the definition file is self-contained, with no file lookup at *run*
  time.
- **Placement within the region / bank window** — the offset each run of
  bytes starts at, so a range that populates only part of its address span
  (e.g. the 8-byte PC-1600 module header at its fixed offset plus a jump
  table, §7, with the gaps left unspecified) can be written sparsely.
  Bytes left unspecified take the range's declared fill pattern (§8),
  conventionally `0xFF`.
- **Multi-bank content** — the section must be able to specify bytes for
  **more than one bank** of the same region: each byte-content block is
  keyed by the bank (or bank range) it fills, so a multi-bank ROM, a
  multi-bank Flash initial image, or the ROM/Flash bank-ranges of a
  mixed-content banked region can all be populated from one file without
  the banks colliding or loading out of order.

A file that marks a region or bank-range D2 = ROM without supplying its
bytes is incomplete and should be rejected at load time, the same way a
header/content pairing that contradicts itself is (§7). A Flash range with
no byte-content block is complete — it just starts erased.

This section is only about content fixed *by the definition*. A separate
preload file that runs BASIC or machine code which then drives the Flash
write protocol (§5) at runtime is an ordinary use of the running machine,
outside this spec's scope, and changes nothing here.

---

## 6. D3/D4 — Banking presence and latch mechanism

- **Unbanked** (D3) — one copy of the content behind the addressing spec,
  no selection needed. This is CE-155, CE-161, CE-159, CE-160, the
  non-banked "maxed out" cards, and — despite Sharp's own module having
  two physical banks — CE-1600M/CE-1620M, because that pair's two-bank
  selection is entirely inside the *host's* native address remapping
  (Port 31H/PU/PT) and never appears as a distinct signal on the module's
  own connector pins to decode at all (§9).
- **Banked** (D3) — more than one copy of the content exists behind the
  same addressing spec (§4), with an active bank selected per D4. Every
  banked region needs a **bank window** (§4's addressing sub-model,
  reused) describing the address span the banking applies within, and a
  **bank count** per D2's content-range list (§5). D4 — the bank latch
  signal — is one of exactly two shapes, and is the *only* field a
  definition file needs to fill in for this; there is no additional
  "orientation" field alongside it (§3):
  - **Trigger-based** — a strobe whose assertion is the *instant* the
    module samples a bank number, decodes it, and latches it internally,
    held until the next such strobe. This is the mechanism **both**
    confirmed banked families use; they differ only in what the strobe is
    and which bus it samples:
    - **CE-163 / CE-163F** — the strobe is a **pin** (pin 18: `S3` on
      PC-1500, `S5` on PC-1500A), a memory-write pulse; the sampled
      source is the **address** bus (`A0` confirmed; `A0`–`A3` for the
      16-bank recreation).
    - **CE-1601M / *superRAM*** — the strobe is an **`OUT (28H)`** I/O
      write (Sharp's "vertical bank register"); the sampled source is the
      **data** bus — the byte written (`D0`–`D2` for CE-1601M's 8-way
      decoder, `D0`–`D3` for *superRAM*'s 4-bit register). Port 28H is
      decoded *on the module*, so the module — not the mainboard — holds
      the latched value.

    Fields: what the trigger is (a pin, or an I/O port), which lines are
    sampled, and — recorded explicitly, never inferred from line names —
    which **domain** (address vs data) they belong to, since the two
    families use different domains and "4 bits" alone doesn't
    disambiguate `A0`–`A3` from `D0`–`D3`. This is what "Vertical" denotes
    in the descriptive terminology (§3).
  - **Line-based** — a named signal or signal group the module reads live
    on every access, with no internal latch at all (the host holds the
    bank number in one of its own registers and drives it continuously
    onto the lines). Field: which line(s), and the **bank data source
    domain** (the address/select-line domain). No confirmed module needs
    this today — CE-1601M/*superRAM* look line-based from the host side
    but latch internally (above), so they are trigger-based — but the
    shape stays expressible for a future module that genuinely wires a
    live host register straight through. This is what "Horizontal"
    denotes in the descriptive terminology (§3).

**Naming trap:** Sharp's own TRM calls the CE-1601M/*superRAM* mechanism
the "vertical bank register" (Port 28H). It is tempting to read "register"
as "the host holds it, so the module just reads a live line" — i.e.
Line-based. But Port 28H is decoded *on the module*: the `OUT (28H)` is a
strobe, and the module samples the data bus and latches the result. By
the test that matters (does the module have to remember something), that
is **Trigger-based**, the same family as CE-163 — the two just sample
different buses (CE-163 the address bus off a pin strobe, CE-1601M the
data bus off an I/O-port strobe). Neither a definition file author nor the
loader ever has to write "Vertical" or "Horizontal": naming the trigger,
the sampled lines, and the domain says everything the file needs to.

---

## 7. PC-1600 module header

The PC-1600 firmware identifies a ROM/RAM-disk module by an **8-byte
header physically present at the start of the module's own address space**
(`8000H`, `A000H`, or `B000H` depending on which page the module occupies)
— `PC-1600-Memory-Bank-Switching.md`'s "ROM Module Detection and Headers":
ID bytes `43H 16H`, reset-jump/checksum, start/boot fields, module
length/boot address, BASIC address, end address, and a type byte
(`80H`/`FFH` = RAM-Disk, `F0H` = Program, `F2H` = System, `01H` =
write-protected), followed by a jump table (reset, interrupt, important
routines, `AUTORUN.BAS` search, device-name pointer, token-table pointer,
file-processing jump) at a fixed offset. Boot-time presence is additionally
tracked host-side via bitmaps at `F0AEH`/`F0AFH` (by start address and
bank position) and probed per-slot via the `SLOTST` firmware call, which
reads the type byte directly out of the header.

This has two consequences for the definition file, not one:

- **The header's bytes are content, not decode configuration.** They live
  inside whatever populates the region — the definition file's own
  byte-content section (§5a) — at the
  fixed offset the real hardware uses — the
  definition file doesn't need fields to spell out the reset-jump address
  or the jump table, because that data is part of the loaded image the
  same way it would be part of a real EPROM's contents. A PC-1600
  region's D1 addressing already puts that image at
  `8000H`/`A000H`/`B000H` correctly if the enable condition is right;
  nothing further is needed for the *bytes themselves* to be correct.
- **The module's class still needs to be a declared field**, separate
  from the header bytes, for two reasons: (a) authoring/validation — the
  loader can sanity-check that a region's (or bank-range's) own declared
  D2 content is consistent with what its header claims (e.g. a range
  marked ROM/read-only but whose header type byte says RAM-Disk is a
  self-contradictory file, worth rejecting or warning on rather than
  silently loading); and (b) because the two host-side presence bitmaps
  (`F0AEH`/`F0AFH`) are populated by the firmware's own boot-time scan
  reading the header live off the module — since that scan is exactly the
  kind of real hardware behavior this project reimplements at the
  connector level rather than shortcuts, the definition file doesn't need
  to duplicate the bitmap
  encoding itself, only to guarantee the header bytes it ships are
  self-consistent so the firmware's real scan logic produces the right
  answer. **A PC-1600 module region should therefore carry an explicit
  "module class" field (Program / System / RAM-Disk / plain-unbanked-
  RAM-no-header) purely as an authoring-time cross-check against the
  header bytes in its loaded content — not as a separate runtime
  mechanism the loader has to implement in addition to just exposing the
  header bytes at the right address.**
- The PC-1500/1500A side has no equivalent header requirement — this
  section is PC-1600-only. (CE-163 is explicitly confirmed to carry *no*
  header at all, per the source doc — "to a boot-time probe it looks like
  plain unbanked SRAM," which is consistent with modeling it as Regular
  content with a trigger-based bank latch (§6) and no module-class field
  set, since that field is PC-1600-specific.)

---

## 8. Initial content / power-up state

Distinct from D1 (addressing) and D2 (content kind) is *what's inside* a
region or bank-range at load time:

- A **Regular, writable** range typically starts as whatever the real
  chip's power-up state is (per this project's own established
  convention — `PC1500Memory`'s RAM arrays power up as `0xFF`, not zero,
  matching real SRAM) — the definition
  file should be able to declare this per range rather than assume the
  emulator's global default is always right for every module (a
  battery-backed module that's been "used" already, for instance, isn't
  meaningfully modeled by a fresh power-up pattern, but that's a content
  question, not addressed further here).
- A **ROM or Flash** range (§5) has no meaningful power-up state of its
  own — any fixed content comes from the definition file's own
  byte-content section (§5a), keyed per bank-range for a banked region so
  a multi-bank card's banks don't collide or load out of order. There is
  no external preset/image file to name. **ROM** must carry this content;
  **Flash** carries it only as the range's *initial* array, and only if
  the file declares one (otherwise the Flash range starts erased, all
  `0xFF`). The running calculator can subsequently overwrite a Flash range
  via the runtime program/erase protocol (§5); reloading the card always
  resets it back to that initial content (or to erased if none was
  declared), the same way it resets a Regular range to its own default
  (above) and a ROM range to its definition-file bytes — a load is a full
  reset of the card regardless of content kind.
- Whether Regular memory's runtime-togglable write-protect (§5, e.g.
  CE-159) starts protected or unprotected by default, and whether that
  state is itself part of the card's persisted state or always resets to
  a fixed default on load, is a content/behavior question the file should
  settle explicitly rather than leaving to an implicit loader default.

---

## 9. Coverage check against the source module table

Every row of `Software-Defined-Memory-Extension.md` §2–3 must be
expressible as one or more regions, each fully placed on D1–D4. Working
through them:

| Module | D1 Addressing | D2 Content | D3 Banking | D4 Latch mechanism | Compatible hosts |
|---|---|---|---|---|---|
| CE-151 | (Y0 AND `AD11–AD13 = 111`) OR S1 — same pattern as CE-155, scaled down | Regular | Unbanked | — | PC-1500, PC-1500A (pins 4, 16 keep the same role on both) |
| CE-155 | (Y0 AND `AD11–AD13 = 111`) OR S1 OR S2 OR S3 — the three S groups are bare mainboard strobes, one per dedicated 2KB chip, no on-module decode | Regular | Unbanked | — | PC-1500, PC-1500A (pins 4, 16, 17, 18 keep the same role on both — §1) |
| CE-157 | RAM: same shape as CE-151/155 area. ROM: likely Y2 sub-range, PV-selected | RAM: Regular. ROM: ROM | RAM: Unbanked. ROM: unconfirmed — possibly banked if PV is a live line | ROM: unconfirmed — flagged in the source doc itself; this spec's job is only to be *capable* of expressing it once confirmed | PC-1500, PC-1500A |
| CE-159 | Same as CE-155 | Regular, write-protect togglable | Unbanked | — | PC-1500, PC-1500A |
| CE-160 | Same territory class as CE-159 (read-only) | ROM | Unbanked | — | PC-1500, PC-1500A |
| CE-161 | Y0 alone, full 16KB | Regular | Unbanked | — | PC-1500, PC-1500A |
| CE-163 | Y0, 16KB window | Regular, all banks | Banked | Trigger-based: pin 18 write pulse; source `A0` (address domain) | PC-1500, PC-1500A |
| 16-bank CE-163-alike | Same as CE-163 | Regular, all banks | Banked | Trigger-based: pin 18; source `A0`–`A3` | PC-1500, PC-1500A |
| CE-1638 | Y0, 16KB window | Regular, all banks | Banked, 8 banks | Trigger-based: pin 18 write pulse; source `A0`–`A2` (address domain) | PC-1500, PC-1500A, PC-1600 Slot 1, PC-1600 Slot 2 — expressed as `Calc-U-1600/Resources/ce1638.card.yaml`; `CE1638PlusCard.hpp` is a separate throwaway test card (invented extra unbanked regions), not this module |
| CE-163F | Y0, 16KB window | Banks 0–7: Regular. Banks 8–F: Flash (bank 8 firmware-reserved, 9–F user-writable) — see §5 | Banked, 16 banks | Trigger-based: pin 18 write pulse; source `A0`–`A3` (address domain, same mechanism as the 16-bank CE-163-alike above) | PC-1500, PC-1500A, PC-1600 Slot 1, PC-1600 Slot 2 — expressed as `Calc-U-1600/Resources/ce163f.card.yaml` (Flash content + `by-bank` split, §5), alongside the still-live hand-written `CE163FCard.hpp` prototype its flash engine was ported from |
| "maxed out," PC-1500/1500A | Y0 full window OR S1 OR S2 OR S3 OR S4 (pin 5 included) — again bare per-strobe groups, OR'd | Regular | Unbanked | — | PC-1500 (full 24KB, all four S-pins live), PC-1500A (author's choice — the pin-5-driven 2KB slice stays dormant there since pin 5 is NC, leaving the same 22KB a PC-1500A-only variant would offer; declaring PC-1500A compatible is optional but not incorrect, §1) |
| CE-1600M | RAM1/RAM2 | Regular | Unbanked (native host bank selection, invisible on the connector) | — | PC-1600 Slot 1, PC-1600 Slot 2 |
| CE-1601M | RAM1 (Slot 2) | Regular, both banks | Banked | Trigger-based: `OUT (28H)` write; source `D0`–`D2` (data domain — the module decodes/latches Port 28H itself); bank window nests a `PVOUT` half-select (D1, one level deeper — see §4) | PC-1600 Slot 2 only (the Port 28H strobe doesn't reach Slot 1) |
| CE-1620M | Same as CE-1600M | ROM | Unbanked | — | PC-1600 Slot 1, PC-1600 Slot 2 |
| PC-1600 Slot 2 "maxed out" (*superRAM*-alike) | RAM1 | Regular, all banks | Banked | Trigger-based: `OUT (28H)` write; source `D0`–`D3` (4-bit register latched on the module); same PVOUT nesting as CE-1601M | PC-1600 Slot 2 only |

Three structural points this table confirms rather than just illustrates:

1. **D1's nesting requirement is real, not hypothetical** — CE-1601M and
   *superRAM* both need a bank-select condition combined with an
   independent `PVOUT` half-select inside whichever bank is currently
   active; the bank window (§6) carries its own full D1 spec rather than a
   bare address range specifically because of these two rows.
2. **CE-163F confirms the split-content case is real, not hypothetical
   either** (§5) — its 8 Regular banks followed by 8 Flash banks, one
   D2 value changing partway through an otherwise-uniform bank range, is
   exactly what §5's `{bank range → content kind}` list form exists for.
   No confirmed module mixes ROM with either of the other two kinds within
   one banked region, or nests Flash inside CE-1601M/*superRAM*-style
   sub-banked territory — those remain unexercised but expressible,
   because D2/D3/D4 being genuinely independent dimensions is what makes
   future hypothetical or modern-recreation cards (an R/W-plus-ROM-bank
   cartridge, say) expressible without a spec change.
3. **The "maxed out" row is one definition covering both hosts, with one
   host running it partially dormant** — compatibility is an author
   choice, not a pin-derived pass/fail (§1), so a single file using pin 5
   can declare both PC-1500 and PC-1500A compatible: it reaches the true
   24KB ceiling on PC-1500, and on PC-1500A the pin-5-driven 2KB slice
   simply never asserts (pin 5 is NC there), leaving the same 22KB a
   PC-1500A-targeted card would offer on its own. An author who instead
   wants a card that behaves identically (no dormant slice) on every host
   it declares compatible can still write one that never references pin
   5 at all — nothing in the mechanism forces either choice, both are
   valid (§1).

---

## 10. Open questions carried forward, not resolved here

- **File format/serialization** — a first concrete pass now exists in
  `Memory-Card-Definition-Format.md` (YAML; key names, the §3 addressing
  grammar, and hex/base64/sidecar encoding of the §5a byte blocks), with
  worked `Calc-U-1600/Resources/*.card.yaml` for CE-155, CE-1600M,
  CE-1601M, CE-1638, and CE-163F (the last two added once Flash content +
  `by-bank` split content moved from "parsed far enough to reject" to
  implemented — Format.md §9). That format is still revisable — this
  content model, not the YAML spelling, is the fixed part.
- **Bundled "standard" resource files** — *implemented for five modules so
  far.* The standard `.card.yaml` files ship under
  `Calc-U-1600/Resources/`, are indexed by `module-name` via
  `Core/Connector/MemoryCardCatalog.hpp`, and are selectable from the
  GUI's control-bar module picker; a preset reaches them with
  `modulespec: <module-name>`. Which further modules ship (presumably at
  least the fully-confirmed rows in §9 — CE-155, CE-159, CE-161, CE-163,
  CE-163F, CE-1600M, CE-1601M) versus which stay example-only is a
  product decision for whoever authors the resource set, not a spec-level
  question.
- **CE-157's ROM half and CE-160's exact 7.8KB reserved layout** are
  themselves unconfirmed in the source research (flagged as such there);
  this spec is written so those regions are *expressible* once confirmed,
  not so their exact parameters are already known.
