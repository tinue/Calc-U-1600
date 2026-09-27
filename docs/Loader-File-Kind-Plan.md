# Loaders: one file classifier from libsharpdx (`sde_file_info`)

**Status: done** (dev-0.6.0, 2026-09-27: phases 0-5 = 484ccad, d9b7127,
4ebbc4a, ec914e8, fbf7fa5 and the docs commit). What changed against the plan
below while implementing:

- The macOS lib is built from the `v0.3.1` tag, not the checkout's HEAD (one
  version bump ahead), so `sde_version()` matches the CI libs.
- `classify()` also reports `truncated`: the library gives no header length
  for a truncated file, so that message says "promises more than the N
  bytes that follow" instead of quoting both lengths.
- The per-machine BASIC entries are `loadBasicProgram(machine, bytes)` plus
  `loadBasicProgramFile(machine, path)`; the tests keep feeding bytes.
- New BAD_PAYLOAD refusal showed up in one test with a malformed record.
- The old real-sample tests pointed at `Core/tests/fixtures/basic/` files
  that never existed; that folder now holds the lissajou `.bas` + `.bbin`.
- The preset log line for `basic-binary` names the file instead of the token
  count (the count is only known inside the loader now). Every other log
  line and every `--dump-basic` matches the pre-change CLIs.

## Context

SharpDataExchange 0.3.1 (released 2026-09-27) adds `sde_file_info`: one
call returns the file's kind (`basic-ascii`, `basic-pc1500`, `basic-pc1600`,
`ml-lh5801`, `ml-z80`, `raw*`, `reserve*`, `variables*`, `text`, `empty`), its problems
(TRUNCATED, TRAILING, LEADING_NOISE, HEADER_CUT, BAD_PAYLOAD), the payload offset and
length, and for machine code the load address, run address and autorun flag.

Calc-U-1600 parses the same headers by hand in three places:
- `Core/Basic/BasicBinaryImage.cpp`: CE-158 and PC-1600 BASIC headers. Only tests
  use it: `loadBasicBinaryProgram()` has no production caller.
- `Core/PC1600/PC1600MachineImage.cpp`: the PC-1600 machine-code header.
- `Core/MachineCodeFile.cpp` `readFile()`: the CE-158 machine-code header, magic
  checks, and the type-byte refusals.

Meanwhile Load BASIC Program… and `format: basic-binary` only accept `.bas` listings.

Decisions (user, 2026-09-27):
- Replace all hand-written header parsing with `sde_file_info`.
- The BASIC loaders also accept tokenized `.bbin` files (CE-158 and PC-1600).
- libsharpdx becomes a hard build dependency. The soft-fail that builds without it
  is removed.
- Kept as they are: the two menu items, the preset `format:` values, and
  `basic-text`.

Rules that stay in force (docs/Loader-Mode-Plan.md, docs/Decisions.md):
- **No CPU guessing.** The `raw-lh5801` / `raw-z80` guess is ignored: every `raw*`
  kind is "headerless", and MODE (or the debugger's `cpu`) still decides the CPU.
- MODE and `TITLE` decide the target; the loaders never change them.
- A header autorun of `0` or `FFFF` means no autorun. This matches the uncommitted
  working-tree change to MachineCodeFile / PC1600MachineImage. The library only
  treats `FFFF` as none, so the wrapper adds the `0` case.

## Phase 0: library refresh (prerequisite)

1. ~~Release SharpDataExchange v0.3.1~~ — done (2026-09-27): the release has the
   Linux x86_64/aarch64, Windows x86_64/aarch64 and macOS arm64 archives.
2. `tools/refresh_sharpdx.sh`: rebuilds the macOS `libsharpdx.a` and `sharpdx.h`
   from the sibling checkout.
3. `tools/fetch_sharpdx.sh`: default `TAG` becomes `v0.3.1`.
4. `Core/Basic/vendor/sharpdx/README.md`: version 0.3.1, and the list of functions
   used gets `sde_file_info`.

## Phase 1: libsharpdx becomes a hard dependency

- `CMakeLists.txt`: move `PresetFile.cpp`, `PresetRunner.cpp`, `*BasicLoader.cpp`,
  `*PresetLoader.cpp` and `Basic/*.cpp` into `CORE_COMMON_SOURCES`. Link
  `${SHARPDX_LIB}` unconditionally into `CoreTests`, `pc1500_cli`, `pc1600_cli` and
  `pc1600_plotter_probe`. If the lib is missing, fail with
  `message(FATAL_ERROR "... run tools/fetch_sharpdx.sh")`.
- `Qt6/CMakeLists.txt`: the same, plus remove the long gating comment and the
  `CALCU1600_PRESET_LOADER_AVAILABLE` definition.
- `Qt6/app/PresetController.{hpp,cpp}`: remove the `#ifdef` and the `#else` stubs.
- `.github/workflows/build.yml`: the "Fetch sharpdx" steps lose
  `continue-on-error`, and their comments stop saying "soft-fail".
- `tools/cloud_session_start.sh`: the fetch failure message now says the build
  fails without the lib.
- Check `docs/Building.md` and `docs/Cloud-Sessions.md` for "builds without
  preset loading" wording.

## Phase 2: one classifier, `Core/ProgramFile.{hpp,cpp}`

A thin C++ wrapper, the only caller of `sde_file_info`:

```cpp
namespace programfile {
enum class Kind { Empty, BasicListing, BasicPC1500, BasicPC1600, CodeLH5801, CodeZ80,
                  Headerless, Other /* reserve*, variables*, text */ };
struct ProgramFile {
    Kind kind; std::string token;          // sde token, for messages
    bool damaged;                          // problems & SDE_PROBLEM_FATAL, except the
                                           // TRUNCATED case below
    bool lengthMismatch;                   // TRUNCATED | TRAILING on a headered file
    std::vector<uint8_t> payload;          // payload_offset .. end of file (see below)
    size_t headerPayloadLen;               // fi.payload_len
    uint32_t loadAddr, autorunAddr;        // code only; autorun 0 = none (0 or FFFF)
};
ProgramFile classify(const std::vector<uint8_t>& bytes);
}
```

- `payload` runs from `payload_offset` to the end of the file, not just
  `payload_len` bytes. That keeps today's rule that a preset's `length:` can
  override a header length mismatch. For machine code, TRUNCATED is a length
  mismatch as it is today, not "damaged". The BASIC path refuses any mismatch, as
  `parseBasicBinaryTransfer` does now.
- `raw`, `raw-lh5801` and `raw-z80` all map to `Headerless`. A comment points at
  the no-guessing rule.
- Bonus behaviour, all from the library: LEADING_NOISE files load (`00` bytes
  before the header). HEADER_CUT and BAD_PAYLOAD (tokenized BASIC that doesn't
  de-tokenize) are refused. The PC-1600 end marker (`00 0F` / `00 F0`) is no
  longer checked.

## Phase 3: machine code on top of `classify()`

- `machinecode::readFile()` (`Core/MachineCodeFile.cpp`) keeps its `File` struct
  and callers (MainWindow, PresetRunner, debug ProgramLoader) but is built from
  `classify()`:
  - `CodeZ80` becomes `Header::PC1600`, `CodeLH5801` becomes `Header::CE158`, and
    `Headerless` becomes `Header::None`.
  - `BasicListing`, `BasicPC1500` and `BasicPC1600` are refused with "…BASIC
    program — use Load BASIC Program". This is new for a `.bas` listing, which
    today loads as raw bytes.
  - `Other` is refused as "<token>, not machine code".
  - Any other damaged file is refused.
  - The length-mismatch message keeps its wording ("header says N bytes, but M
    follow it").
- Deleted: `hasPc1600Magic`, `hasCe158Magic`, `be16`, the `kCe158*` /
  `kPc1600Type*` constants, `Core/PC1600/PC1600MachineImage.{hpp,cpp}`, and
  `Core/tests/pc1600_machine_image_tests.cpp`. Its useful cases (autorun
  `FFFF` / `FFFFFF` / `0`, length not checked by the parser) go into
  `machine_code_file_tests.cpp`, which already has the `FFFF` test.
- Remove the deleted files from both CMakeLists and from `tools/build_*.sh`.

## Phase 4: BASIC loaders accept listings and tokenized files

- `Core/Basic/BasicProgramSource.{hpp,cpp}` becomes the single file reader:
  `readBasicProgram(path, TransferModel listingModel)` returns
  `{ok, payload, TransferModel source, error}`:
  - `BasicListing` is tokenized with `listingModel`, as today (`sde_tokenize`,
    `SDE_SEGMENT_MARKER_MEMORY`).
  - `BasicPC1500` and `BasicPC1600` use the header's payload, with
    `source = PC1500 / PC1600`.
  - A code kind gets "machine code — use Load Machine Code". A mismatch, damaged
    file, `Other` or `Empty` is refused with a specific message.
- Delete `Core/Basic/BasicBinaryImage.{hpp,cpp}` and fold the `TransferModel` enum
  into `BasicProgramSource.hpp`. `basic_binary_image_tests.cpp` becomes classifier
  tests for the BASIC kinds, or is merged into `basic_program_source_tests.cpp`.
- One load entry per machine, taking a path, which does the model check that
  `loadBasicBinaryProgram` does today:
  - `PC1500BasicLoader`: `loadBasicProgramFile(machine, path)` refuses a PC-1600
    source.
  - `PC1600BasicLoader`: `loadBasicListing()` is renamed to
    `loadBasicProgramFile(machine, path)`. It uses `pc1600ListingModel()` for
    listings and refuses a CE-158 source unless `mode1()`; a PC-1600 source loads
    in both MODEs (Loader-Mode-Plan table). The MODE 1 note on tokenizer errors
    stays.
  - `loadBasicBinaryProgram(machine, bytes)` is removed. Tests that used it go
    through the new entry, from a temp file or a bytes overload, whichever is
    simpler.
- Callers:
  - `PresetController::loadBasicProgramLive` calls `loadBasicProgramFile` for both
    models. The `loadBasicProgramLiveOn` template is removed.
  - `PresetRunner` `format: basic-binary` calls a new `PresetMachine::loadBasicFile(path)`,
    which replaces `transferModel()` + `loadBasicPayload()` in `PresetRunner.hpp`
    and both `*PresetLoader.cpp`.
  - The Load BASIC Program dialog filter becomes
    `BASIC Programs (*.bas *.bbin);;All Files (*)`.

## Phase 5: docs

- `docs/User-Guide.md` §4 and §6 and the preset `basic-binary` row: listings or
  tokenized `.bbin` files, and the MODE rule for CE-158 files.
- `docs/Decisions.md` → "Loading programs": file kinds come from `sde_file_info`;
  the library's `raw-lh5801` / `raw-z80` guess is deliberately ignored; header
  autorun `0` or `FFFF` means none.
- `CHANGELOG.md` entry.
- Commit this plan as `docs/Loader-File-Kind-Plan.md`.
- Update the memory note on the fast basic-binary loader.

One commit per phase on `dev-0.6.0`. The pending autorun change in the working tree
goes into Phase 3's commit, since it is superseded or moved there.

## Verification

- CoreTests green after every phase (CMake build plus `tools/build_*.sh` builds).
- New or adjusted tests:
  - classify(): each kind; LEADING_NOISE loads; TRUNCATED/TRAILING make a length
    mismatch; HEADER_CUT is refused; autorun `0` / `FFFF` / `FFFFFF` read as none.
  - readFile(): a `.bas` listing and tokenized BASIC are refused with the Load BASIC
    hint; reserve and text are refused.
  - BASIC:
    - a `.bbin` PC-1500 file loads on the PC-1500 and on the PC-1600 in MODE 1;
    - that file is refused on the PC-1600 in MODE 0;
    - a PC-1600 `.bbin` loads in MODE 0 and MODE 1 and is refused on the PC-1500;
    - `--dump-basic` of a loaded `.bbin` equals the `.bas` load of the same program.
  - The existing Loader-Mode-Plan tests are unchanged.
- In the app (the `run` skill, `-ApplePersistenceIgnoreState YES`, quit via
  `calcu1600/quit`):
  - Load BASIC Program with a `.bbin` from `sde put --dry-run`, then `LIST`.
  - Load Machine Code with `examples/machine-code/memtest.bin` (with a header) and a
    headerless `.bin`; the advice matches today's.
  - Load Machine Code with a `.bas`: refused with the hint.
- Presets: run the `examples/` and `dev/presets/` presets that use
  `basic-binary` / `binary` with the CLIs; the logs match today's.
