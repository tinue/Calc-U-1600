#pragma once
#include <array>
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
// What a file is comes from programfile::classify() (libsharpdx).

namespace machinecode {

enum class Target { PC1500, PC1600 };  // PC1500 also covers the PC-1500A

// PC-1600 load targets: S0 = internal RAM ($C000-$FFFF), S1/S2 = the two
// memory slots ($8000-$BFFF window) -- the same windows as the preset
// loader's machine-code `program: file:`. "Load Machine Code…" only ever
// loads into BASIC's program area (the one `NEW "S0:"` reserves in), so S1/S2 are
// used only when a module is folded into that area as extension memory.
enum class Slot { S0, S1, S2 };
const char* slotName(Slot slot);

// The global bank the $8000-$BFFF window shows for code in `slot` when the
// file names none: bank 0 (slot 1's lower half; S0 needs none), bank 2 for
// slot 2 (Ref/PC-1600/PC-1600-Memory-Architecture.md §4). `CALL #bank,&addr` for bank != 0.
int pc1600DefaultBank(Slot slot);

// The memory slot behind global `bank` 0-3: banks 0/1 slot 1, 2/3 slot 2.
Slot pc1600BankSlot(int bank);

struct File {
    enum class Header { None, CE158, PC1600 };
    Header header = Header::None;
    bool ok = false;               // false => `error` says why (bad/unsupported header)
    std::string error;
    uint32_t loadAddr = 0;         // header only
    uint32_t autorunAddr = 0;      // header only; 0 = none (the header held 0 or &FFFF)
    std::vector<uint8_t> payload;  // the bytes to load (header stripped)
    /// The header's length field disagrees with the bytes that follow it.
    /// `ok` is false and `error` says so, but `loadAddr`/`autorunAddr` and
    /// `payload` (everything after the header) are still filled in, so a
    /// preset's explicit `length:` can override the header.
    bool lengthMismatch = false;
};

// Recognises the header and splits off the payload. A BASIC program
// (listing or tokenized), a Reserve Area, variables or plain text is an
// error, and so is a header whose length doesn't match the file; any other
// binary is a headerless payload -- its CPU is never guessed. Shared by
// every machine-code load (Load Machine Code…, a machine-code `program: file:`, the
// debugger).
File readFile(const std::vector<uint8_t>& bytes);

// Empty when `file`'s header (or lack of one) suits `target`; otherwise why
// not -- a PC-1600 file on a PC-1500, a CE-158 (PC-1500) file on a PC-1600
// that isn't in MODE 1.
std::string headerMismatch(Target target, const File& file, bool mode1 = false);

// One run of the PC-1600's live BASIC program area ("S0"), in the order the
// ROM lays a program down (ADTBL order). With a RAM module fitted the area
// starts in the module and ends in internal RAM; `NEW "S0:",<size>` always
// reserves from the start of the FIRST run. See pc1600BasicAreas().
struct BasicArea {
    int slot = 0;              // 0 = internal RAM, 1 / 2 = that slot's module
    uint32_t windowBase = 0;   // $C000, or the module window base (usually $8000)
    uint32_t top = 0;          // last usable address (inclusive)
    int bank = 0;              // module only: the global bank 0-3 behind $8000-$BFFF
};

// The CPU whose address space a load address is given in. On the PC-1600
// the LH5803's $0000-$7FFF is the Z-80's $8000-$FFFF (pc1600::lh5803ToZ80).
enum class Cpu { Z80, LH5803 };

// What the PC-1600 placement rules read from the running machine
// (docs/background/plans/Loader-Mode-Plan.md) -- see pc1600LoadState().
// The loaders never change MODE or TITLE; they follow them.
struct PC1600State {
    bool mode1 = false;                 // BMODE b6: MODE 1 (PC-1500 compatible)
    int title = 0;                      // F1D5H: the selected program area, 0 S0, 1 S1, 2 S2
    std::vector<BasicArea> basicAreas;  // S0's runs, ADTBL order (see pc1600BasicAreas())
    uint32_t titleBase = 0;             // TITLE 1/2: the program module's window base (Z-80)
    uint32_t titleStart = 0;            // TITLE 1/2: its program start (Z-80, descriptor +4/+5)
    uint32_t ramEnd = 0;                // Z-80 address of the S0 user-area top, (F864H):00
    // Writable RAM per global bank 0..3 behind $8000-$BFFF: bit i set =
    // page $80+i (256 bytes) is RAM. For a header that names a bank.
    std::array<uint64_t, 4> bankRamPages{};
};

// The PC-1600 target for `len` bytes at the Z-80 address `busAddr`: S0 for
// $C000-$FFFF; for $8000-$BFFF the module behind the selected program area
// -- the S1/S2 program module TITLE selects, or with TITLE S0 the module
// folded into S0 (its first run). False => `why` explains, naming addresses
// in `cpu`'s space (the one the user typed).
bool pc1600TargetFor(uint32_t busAddr, size_t len, const PC1600State& state, Slot* slot, std::string* why,
                     Cpu cpu = Cpu::Z80);

// Where headerless PC-1600 code goes by default, as a Z-80 address: the
// start of the selected program area -- S0: its first run's window base +
// &C5 (&C0C5 on a bare machine, &80C5 with a module folded in); S1 / S2:
// the program module's program start.
uint32_t pc1600DefaultAddress(const PC1600State& state);

// A warning when [busAddr, busAddr + len) reaches into the PC-1600's work
// area &F000-&FFFF: the system work area, the WAKE$ strings, or the CE-1F01A
// area. Not a refusal -- many PC-1600 programs live up there, above all in the
// CE-1F01A bar-code reader pen's area &FF40-&FFFF (e.g. CLOCK.BIN, &FF3A-&FFFB,
// which also reaches into WAKE$). `cpu` words it (addresses in the LH5803 view
// for LH5801 code). Empty: nothing to say.
std::string pc1600WorkAreaWarning(uint32_t busAddr, size_t len, Cpu cpu);

struct Plan {
    std::string error;          // non-empty => refuse to load
    bool needsAddress = false;  // headerless: ask for the start address
    uint32_t defaultAddr = 0;   // headerless PC-1600: proposed start address in `cpu`'s space (0 = none)
    Cpu cpu = Cpu::Z80;         // PC-1600: the address space of the file's addresses
    Slot slot = Slot::S0;       // PC-1600 with a header: where the code goes
    int bank = 0;               // PC-1600 with a header: the global bank behind $8000-$BFFF (see LoadPlan)
    uint32_t busAddr = 0;       // PC-1600 with a header: the Z-80 address it goes to
};

// ── One load pipeline ─────────────────────────────────────────────────────
//
// Every machine-code load -- Load Machine Code…, a preset's machine-code
// `program: file:`, the debugger's Build & Load -- plans with planLoad(): the file
// checks, the CPU, the address and length, the range and (PC-1600) the
// target. On the PC-1600 the MODE and TITLE decide
// (docs/background/plans/Loader-Mode-Plan.md): a PC-1600 header means Z-80
// code, a CE-158 header LH5801 code (MODE 1 only), a headerless file the
// MODE's CPU (MODE 0 Z-80, MODE 1 LH5801) -- unless the caller knows the
// CPU (the debugger's toolchain). The target follows from the address and
// the selected program area. The callers write plan.busAddr/len with their
// own writer and word the LoadError their own way.

struct LoadOptions {
    Target target = Target::PC1500;
    bool acceptLengthMismatch = false;  // load a header whose length disagrees with the file
    bool hasAddress = false;            // overrides the header's load address (in the CPU's space)
    uint32_t address = 0;
    bool hasLength = false;             // overrides the payload size (at most the payload)
    size_t length = 0;
    bool hasCpu = false;                // PC-1600: the caller knows the CPU (the debugger); else the file / MODE decide
    Cpu cpu = Cpu::Z80;
    bool checkRange = true;             // refuse a range past $FFFF here (else the writer does)
};

enum class LoadError {
    None,
    BadFile,        // readFile() refused it (detail: its error; the file's lengthMismatch tells a length mismatch)
    HeaderMismatch, // detail: headerMismatch()
    Empty,          // no bytes to load
    NeedsAddress,   // headerless and no address given (defaultAddr: a PC-1600 proposal)
    LengthExceeds,  // `length` is more than the payload
    OutsideBank0,   // the address is above $FFFF
    LhRange,        // LH5803 code outside its 0000-7FFF
    NoSlot,         // detail: pc1600TargetFor()'s reason, or why the header's bank can't take the code
    PastEnd,        // the code runs past $FFFF
};

struct LoadPlan {
    LoadError error = LoadError::None;
    std::string detail;
    Cpu cpu = Cpu::Z80;        // PC-1600: the address space of `addr`
    uint32_t addr = 0;         // in `cpu`'s address space
    uint32_t busAddr = 0;      // where the bytes go: the Z-80 address
    size_t len = 0;
    Slot slot = Slot::S0;
    // PC-1600: the global bank the code sits in -- the header's bank 1-3,
    // else pc1600DefaultBank(slot). A header bank 0 means "none given":
    // the program area decides, as for BLOAD without `#bank`.
    int bank = 0;
    uint32_t defaultAddr = 0;  // NeedsAddress on a PC-1600, in `cpu`'s space
};

// `state`: PC-1600 only (see pc1600LoadState()).
LoadPlan planLoad(const File& file, const LoadOptions& options, const PC1600State& state);

// What to do with `file` on the running `target` (Load Machine Code…):
// header errors, whether an address must be asked, and for a header file
// the CPU, target and Z-80 address. `state`: PC-1600 only.
Plan plan(Target target, const File& file, const PC1600State& state);

// Shown after a successful load.
struct Advice {
    std::string newCommand;   // e.g. `NEW "S0:",&140` / `NEW &4105` -- empty when NEW can't protect the code
    std::string newNote;      // explanation / warning for the NEW line
    std::string callCommand;  // e.g. `CALL &C0C5` / `CALL #2,&8100` / `XCALL &40C5`
    std::string callNote;     // where the CALL address comes from
};

// `addr` / `autorunAddr` are in `cpu`'s address space. `ramStart` / `ramEnd`
// (PC-1500 only): the user-RAM range from RAM_ST / RAM_END ($7863 / $7864,
// page numbers * 256; `ramEnd` is the first address past RAM). `state`
// (PC-1600 only): MODE, TITLE and the program areas -- LH5801 code starts
// with XCALL; in MODE 1 the PC-1500 style `NEW &addr` protects code in S0,
// in MODE 0 `NEW "S0:",size`; code in the selected S1/S2 module gets
// `NEW "Sn:",size`.
// `bank` (PC-1600): the plan's bank; a non-zero bank gives `CALL #bank,`.
Advice advice(Target target, Slot slot, int bank, uint32_t addr, size_t len, uint32_t autorunAddr, uint32_t ramStart,
              uint32_t ramEnd, const PC1600State& state, Cpu cpu);

// Parses a user-typed hex address: `C000`, `&C000`, `$C000`, `0xC000`
// (surrounding blanks ignored). False on anything else or > $FFFF.
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
