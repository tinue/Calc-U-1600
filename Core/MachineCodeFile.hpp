#pragma once
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ── Machine-code `.bin` files for the GUI's "Load Machine Code…" ────────
//
// Qt-free decision logic behind the dialog, kept here so it is unit-
// testable: recognise the file (CE-158 header, PC-1600 header, or none),
// decide what still has to be asked (the start address of a raw file) and
// where PC-1600 code goes (always BASIC's program area, "S0"), and
// work out the advice shown after loading (the NEW that protects the code,
// the CALL that starts it). The writes themselves live in
// PC1500MachineCodeLoader / PC1600MachineCodeLoader.
//
// Header layouts: SharpPC1500Reference Data-Formats/Binary-Exchange-
// Formats.md §2 (CE-158, PC-1500/1500A) and §3 (PC-1600).

namespace machinecode {

enum class Target { PC1500, PC1600 };  // PC1500 also covers the PC-1500A

// PC-1600 load targets: S0 = internal RAM ($C000-$FFFF), S1/S2 = the two
// memory slots ($8000-$BFFF window) -- the same windows as the preset
// loader's `format: binary`. "Load Machine Code…" only ever loads into
// BASIC's program area (the one `NEW "S0:"` reserves in), so S1/S2 are
// used only when a module is folded into that area as extension memory.
enum class Slot { S0, S1, S2 };
const char* slotName(Slot slot);

struct File {
    enum class Header { None, CE158, PC1600 };
    Header header = Header::None;
    bool ok = false;               // false => `error` says why (bad/unsupported header)
    std::string error;
    uint32_t loadAddr = 0;         // header only
    uint32_t autorunAddr = 0;      // header only; 0 = none
    std::vector<uint8_t> payload;  // the bytes to load (header stripped)
    /// The header's length field disagrees with the bytes that follow it.
    /// `ok` is false and `error` says so, but `loadAddr`/`autorunAddr` and
    /// `payload` (everything after the header) are still filled in, so a
    /// preset's explicit `length:` can override the header.
    bool lengthMismatch = false;
};

// Recognises the header and splits off the payload. A header that is
// present but not machine language (e.g. tokenized BASIC) or whose length
// doesn't match the file is an error; anything without a known magic is a
// headerless payload. Shared by the GUI's "Load Machine Code…" and the
// preset loaders' `format: binary`.
File readFile(const std::vector<uint8_t>& bytes);

// Empty when `file`'s header (or lack of one) suits `target`; otherwise why
// not -- a CE-158 file on a PC-1600, a PC-1600 file on a PC-1500.
std::string headerMismatch(Target target, const File& file);

// One run of the PC-1600's live BASIC program area ("S0"), in the order the
// ROM lays a program down (ADTBL order). With a RAM module fitted the area
// starts in the module and ends in internal RAM; `NEW "S0:",<size>` always
// reserves from the start of the FIRST run. See pc1600BasicAreas().
struct BasicArea {
    int slot = 0;              // 0 = internal RAM, 1 / 2 = that slot's module
    uint32_t windowBase = 0;   // $C000, or the module window base (usually $8000)
    uint32_t top = 0;          // last usable address (inclusive)
    uint32_t imageOffset = 0;  // module only: card-image offset of windowBase
};

// The PC-1600 target for `len` bytes at `addr`, always inside BASIC's
// program area (`basicAreas`, see pc1600BasicAreas()): S0 for
// $C000-$FFFF; for $8000-$BFFF the slot whose module is the area's first
// run (a module typed as extension memory). False => `why` explains --
// e.g. $8000-$BFFF with the area starting in internal RAM, which would
// mean a program module or RAM disk: a job for a preset.
bool pc1600TargetFor(uint32_t addr, size_t len, const std::vector<BasicArea>& basicAreas, Slot* slot,
                     std::string* why);

// Where headerless PC-1600 code goes by default: the start of the area
// `NEW "S0:"` can reserve -- the first run's window base + &C5 (&C0C5 on a
// bare machine, &80C5 with a RAM module folded in). &C0C5 if unknown.
uint32_t pc1600DefaultAddress(const std::vector<BasicArea>& basicAreas);

struct Plan {
    std::string error;          // non-empty => refuse to load
    bool needsAddress = false;  // headerless: ask for the start address
    uint32_t defaultAddr = 0;   // headerless PC-1600: proposed start address (0 = none)
    Slot slot = Slot::S0;       // PC-1600 with a header: where the code goes
};

// ── One load pipeline ─────────────────────────────────────────────────────
//
// Every machine-code load -- Load Machine Code…, a preset's `format:
// binary`, the debugger's Build & Load -- plans with planLoad(): the file
// checks, the address and length, the range and (PC-1600) the slot. The
// callers differ only in LoadOptions, then write plan.busAddr/len with their
// own writer and word the LoadError their own way.

// How a PC-1600 load picks its slot.
enum class SlotPolicy {
    Derive,             // pc1600TargetFor() on the BASIC program area; refuse if it has none
    DeriveOrInternal,   // the same, but internal RAM (>= $C000) is always fine
    Explicit,           // LoadOptions::slot as given
};

struct LoadOptions {
    Target target = Target::PC1500;
    bool acceptLengthMismatch = false;  // load a header whose length disagrees with the file
    bool hasAddress = false;            // overrides the header's load address
    uint32_t address = 0;
    bool hasLength = false;             // overrides the payload size (at most the payload)
    size_t length = 0;
    bool lh5803 = false;                // PC-1600: `address` is the LH5803's (its 0000-7FFF = the Z-80's 8000-FFFF)
    bool checkRange = true;             // refuse a range past $FFFF here (else the writer does)
    SlotPolicy slotPolicy = SlotPolicy::Explicit;
    Slot slot = Slot::S0;               // Explicit
};

enum class LoadError {
    None,
    BadFile,        // readFile() refused it (detail: its error; plan.file.lengthMismatch tells a length mismatch)
    HeaderMismatch, // detail: headerMismatch()
    Empty,          // no bytes to load
    NeedsAddress,   // headerless and no address given (defaultAddr: a PC-1600 proposal)
    LengthExceeds,  // `length` is more than the payload
    OutsideBank0,   // the address is above $FFFF
    LhRange,        // LH5803 code outside its 0000-7FFF
    NoSlot,         // detail: pc1600TargetFor()'s reason
    PastEnd,        // the code runs past $FFFF
};

struct LoadPlan {
    LoadError error = LoadError::None;
    std::string detail;
    File file;
    uint32_t addr = 0;         // in the requested CPU's address space
    uint32_t busAddr = 0;      // where the bytes go: `addr`, or the Z-80 address of LH5803 code
    size_t len = 0;
    Slot slot = Slot::S0;
    uint32_t defaultAddr = 0;  // NeedsAddress on a PC-1600
};

// `basicAreas`: PC-1600 only (see pc1600BasicAreas()).
LoadPlan planLoad(const File& file, const LoadOptions& options, const std::vector<BasicArea>& basicAreas);

// What to do with `file` on the running `target`: model mismatch errors
// (a CE-158 file on a PC-1600, a PC-1600 file on a PC-1500), whether an
// address must be asked, and (PC-1600) the target for a header address --
// see pc1600TargetFor(). `basicAreas`: PC-1600 only.
Plan plan(Target target, const File& file, const std::vector<BasicArea>& basicAreas);

// Shown after a successful load.
struct Advice {
    std::string newCommand;   // e.g. `NEW "S0:",&140` -- empty when NEW can't protect the code
    std::string newNote;      // explanation / warning for the NEW line
    std::string callCommand;  // e.g. `CALL &C0C5` / `CALL #2,&8100`
    std::string callNote;     // where the CALL address comes from
};

// `ramStart` / `ramEnd` (PC-1500 only): the user-RAM range from RAM_ST /
// RAM_END ($7863 / $7864, page numbers * 256; `ramEnd` is the first
// address past RAM). `basicAreas` (PC-1600 only): the live BASIC program
// area -- NEW is only advised when the code sits in its first run.
Advice advice(Target target, Slot slot, uint32_t addr, size_t len, uint32_t autorunAddr, uint32_t ramStart,
              uint32_t ramEnd, const std::vector<BasicArea>& basicAreas);

// Parses a user-typed hex address: `C000`, `&C000`, `$C000`, `0xC000`
// (surrounding blanks ignored). False on anything else or > $FFFF.
// Header-inline so PresetFile.cpp can share it without linking the rest.
inline bool parseHexAddress(const std::string& text, uint32_t* out) {
    size_t i = 0, n = text.size();
    while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) i++;
    while (n > i && std::isspace(static_cast<unsigned char>(text[n - 1]))) n--;
    if (i < n && (text[i] == '&' || text[i] == '$')) {
        i++;
    } else if (n - i >= 2 && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
        i += 2;
    }
    if (i == n || n - i > 6) return false;
    uint32_t v = 0;
    for (; i < n; i++) {
        const char c = text[i];
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        v = v * 16 + static_cast<uint32_t>(std::isdigit(static_cast<unsigned char>(c))
                                               ? c - '0'
                                               : std::toupper(static_cast<unsigned char>(c)) - 'A' + 10);
    }
    if (v > 0xFFFF) return false;
    *out = v;
    return true;
}

}  // namespace machinecode
