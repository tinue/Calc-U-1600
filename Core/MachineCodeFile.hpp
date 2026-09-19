#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ── Machine-code `.bin` files for the GUI's "Load Machine Code…" ────────
//
// Qt-free decision logic behind the dialog, kept here so it is unit-
// testable: recognise the file (CE-158 header, PC-1600 header, or none),
// decide what still has to be asked (start address, PC-1600 slot), and
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
// loader's `format: binary`.
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
};

// Recognises the header and splits off the payload. A header that is
// present but not machine language (e.g. tokenized BASIC) or whose length
// doesn't match the file is an error; anything without a known magic is a
// headerless payload.
File readFile(const std::vector<uint8_t>& bytes);

// PC-1600 slots that can hold `len` bytes at `addr`: S0 for $C000-$FFFF,
// the attached ones of S1/S2 for $8000-$BFFF. Empty => `why` explains.
std::vector<Slot> pc1600SlotsFor(uint32_t addr, size_t len, bool slot1Attached, bool slot2Attached,
                                 std::string* why);

struct Plan {
    std::string error;              // non-empty => refuse to load
    bool needsAddress = false;      // headerless: ask for the start address
    std::vector<Slot> slotChoices;  // PC-1600 with a header: the slots that fit (ask when > 1)
};

// What to do with `file` on the running `target`: model mismatch errors
// (a CE-158 file on a PC-1600, a PC-1600 file on a PC-1500), whether an
// address must be asked, and the fitting PC-1600 slots for a header address.
Plan plan(Target target, const File& file, bool slot1Attached, bool slot2Attached);

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
bool parseHexAddress(const std::string& text, uint32_t* out);

}  // namespace machinecode
