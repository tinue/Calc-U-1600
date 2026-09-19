#include "MachineCodeFile.hpp"

#include <cctype>
#include <cstdio>

#include "PC1600/PC1600MachineImage.hpp"

namespace machinecode {

namespace {

constexpr size_t kCe158HeaderSize = 27;
constexpr uint8_t kCe158TypeMachine = 0x42;  // 'B'
constexpr uint8_t kPc1600TypeMachine = 0x10;
constexpr uint8_t kPc1600TypeBasic = 0x21;

// The system keeps the first &C5 (197) bytes of a program area for itself
// -- PC-1500 BASIC can't start lower (`NEW 0` == RAM start + &C5), and a
// PC-1600 `NEW "Sx:"` reserve starts at base + &C5.
constexpr uint32_t kReserve = 0xC5;

constexpr uint32_t kPc1600SlotBase = 0x8000;
constexpr uint32_t kPc1600S0Base = 0xC000;
constexpr uint32_t kPc1600WorkArea = 0xF000;  // F000-FFFF: BASIC/IOCS work area
// Top of the work area (TRM §6.1 block E, German manual's work-area dump):
// FF00-FF3F holds WAKE$(0)/WAKE$(1), FF40-FFFF is unused by the system but
// reserved for the CE-1F01A bar-code reader software -- the de-facto home
// of many small published PC-1600 routines.
constexpr uint32_t kPc1600Wake = 0xFF00;
constexpr uint32_t kPc1600FreeTop = 0xFF40;

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "&%X", v);
    return b;
}

bool hasPc1600Magic(const std::vector<uint8_t>& d) {
    return d.size() >= 16 && d[0] == 0xFF && d[1] == 0x10 && d[2] == 0x00 && d[3] == 0x00;
}

bool hasCe158Magic(const std::vector<uint8_t>& d) {
    return d.size() >= kCe158HeaderSize && d[0] == 0x01 && d[2] == 'C' && d[3] == 'O' && d[4] == 'M';
}

uint32_t be16(const std::vector<uint8_t>& d, size_t at) {
    return (static_cast<uint32_t>(d[at]) << 8) | d[at + 1];
}

}  // namespace

const char* slotName(Slot slot) {
    switch (slot) {
        case Slot::S0: return "S0";
        case Slot::S1: return "S1";
        case Slot::S2: return "S2";
    }
    return "S0";
}

File readFile(const std::vector<uint8_t>& bytes) {
    File f;
    if (hasPc1600Magic(bytes)) {
        f.header = File::Header::PC1600;
        if (bytes[4] == kPc1600TypeBasic) {
            f.error = "This is a tokenized PC-1600 BASIC program, not machine code -- use Load BASIC Program.";
            return f;
        }
        if (bytes[4] != kPc1600TypeMachine) {
            char b[96];
            std::snprintf(b, sizeof(b), "PC-1600 transfer file of type &%02X is not machine code.", bytes[4]);
            f.error = b;
            return f;
        }
        pc1600::MachineImage img = pc1600::parsePC1600MachineImage(bytes);
        if (!img.ok) {
            f.error = img.error;
            return f;
        }
        const size_t avail = bytes.size() - img.headerSize;
        if (img.headerPayloadLen != avail) {
            char b[160];
            std::snprintf(b, sizeof(b), "The PC-1600 header says %u bytes of code, but %zu follow it.",
                          img.headerPayloadLen, avail);
            f.error = b;
            return f;
        }
        f.loadAddr = img.loadAddr;
        f.autorunAddr = img.autorunAddr;
        f.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(img.headerSize), bytes.end());
        f.ok = true;
        return f;
    }
    if (hasCe158Magic(bytes)) {
        f.header = File::Header::CE158;
        if (bytes[1] != kCe158TypeMachine) {
            char b[96];
            std::snprintf(b, sizeof(b), "CE-158 transfer file of type '%c' (&%02X) is not machine code.",
                          std::isprint(bytes[1]) ? bytes[1] : '?', bytes[1]);
            f.error = b;
            return f;
        }
        // Length field is stored as (length - 1) -- Binary-Exchange-Formats.md §2.3.
        const size_t len = static_cast<size_t>(be16(bytes, 0x17)) + 1;
        const size_t avail = bytes.size() - kCe158HeaderSize;
        if (len != avail) {
            char b[160];
            std::snprintf(b, sizeof(b), "The CE-158 header says %zu bytes of code, but %zu follow it.", len, avail);
            f.error = b;
            return f;
        }
        f.loadAddr = be16(bytes, 0x15);
        f.autorunAddr = be16(bytes, 0x19);
        f.payload.assign(bytes.begin() + kCe158HeaderSize, bytes.end());
        f.ok = true;
        return f;
    }
    f.payload = bytes;
    f.ok = true;
    return f;
}

bool pc1600TargetFor(uint32_t addr, size_t len, const std::vector<BasicArea>& basicAreas, Slot* slot,
                     std::string* why) {
    const uint64_t end = static_cast<uint64_t>(addr) + len;  // one past the last byte
    if (addr >= kPc1600S0Base) {
        if (end > 0x10000) {
            if (why) *why = hex(addr) + " + " + std::to_string(len) + " bytes runs past &FFFF.";
            return false;
        }
        *slot = Slot::S0;
        return true;
    }
    if (addr < kPc1600SlotBase) {
        if (why) *why = hex(addr) + " is ROM -- machine code goes into BASIC's program area, from " +
                        hex(pc1600DefaultAddress(basicAreas)) + ".";
        return false;
    }
    if (end > kPc1600S0Base) {
        if (why)
            *why = hex(addr) + " + " + std::to_string(len) +
                   " bytes crosses &C000 -- a memory module's window is &8000-&BFFF.";
        return false;
    }
    // $8000-$BFFF is part of BASIC's program area only when a RAM module
    // is folded into it (extension memory, no module header) -- then that
    // module is the area's first run. A program module or RAM disk isn't
    // (a NEW "S1:"/"S2:" reserve there needs INIT "Sx:","P" first, which
    // a preset can do).
    if (basicAreas.empty()) {
        if (why) *why = "BASIC's program area couldn't be read, so " + hex(addr) + " can't be placed.";
        return false;
    }
    const BasicArea& first = basicAreas.front();
    if (first.slot == 0) {
        if (why)
            *why = hex(addr) + " is in a memory module, which isn't part of BASIC's program area here -- that "
                   "starts in internal RAM at " + hex(first.windowBase + kReserve) +
                   ". To load code into a program module, use a preset (INIT\"Sx:\",\"P\", then "
                   "NEW\"Sx:\",n and `format: binary`).";
        return false;
    }
    if (addr < first.windowBase) {
        if (why) *why = hex(addr) + " is below the slot " + std::to_string(first.slot) + " module, which starts at " +
                        hex(first.windowBase) + ".";
        return false;
    }
    *slot = first.slot == 1 ? Slot::S1 : Slot::S2;
    return true;
}

uint32_t pc1600DefaultAddress(const std::vector<BasicArea>& basicAreas) {
    return (basicAreas.empty() ? kPc1600S0Base : basicAreas.front().windowBase) + kReserve;
}

Plan plan(Target target, const File& file, const std::vector<BasicArea>& basicAreas) {
    Plan p;
    if (!file.ok) {
        p.error = file.error;
        return p;
    }
    if (file.payload.empty()) {
        p.error = "The file contains no code.";
        return p;
    }
    if (target == Target::PC1600 && file.header == File::Header::CE158) {
        p.error = "This file has a CE-158 (PC-1500) header. Loading PC-1500 machine code into the PC-1600's "
                  "LH5803 side is not supported yet.";
        return p;
    }
    if (target == Target::PC1500 && file.header == File::Header::PC1600) {
        p.error = "This file has a PC-1600 header and can't be loaded into a PC-1500.";
        return p;
    }
    if (file.header == File::Header::None) {
        p.needsAddress = true;
        if (target == Target::PC1600) p.defaultAddr = pc1600DefaultAddress(basicAreas);
        return p;
    }
    if (target == Target::PC1500) {
        if (static_cast<uint64_t>(file.loadAddr) + file.payload.size() > 0x10000)
            p.error = "The code (" + std::to_string(file.payload.size()) + " bytes at " + hex(file.loadAddr) +
                      ") runs past &FFFF.";
        return p;
    }
    if (file.loadAddr > 0xFFFF) {
        p.error = "The header's load address " + hex(file.loadAddr) +
                  " is outside bank 0; banked loads are not supported.";
        return p;
    }
    std::string why;
    if (!pc1600TargetFor(file.loadAddr, file.payload.size(), basicAreas, &p.slot, &why))
        p.error = "The header's load address doesn't fit: " + why;
    return p;
}

namespace {

const char* areaWhere(int slot) {
    return slot == 1 ? " in the slot 1 module" : slot == 2 ? " in the slot 2 module" : " in internal RAM";
}

// Whether `slot`'s load window maps [addr, end) onto the memory of `area`:
// same physical target, and (for a module) the same card-image bytes --
// Slot loads write card-image offset (addr - $8000).
bool sameMemory(const BasicArea& area, Slot slot, uint32_t addr) {
    const int wanted = slot == Slot::S0 ? 0 : slot == Slot::S1 ? 1 : 2;
    if (area.slot != wanted) return false;
    if (wanted == 0) return true;
    return addr >= area.windowBase && area.imageOffset + (addr - area.windowBase) == addr - kPc1600SlotBase;
}

}  // namespace

Advice advice(Target target, Slot slot, uint32_t addr, size_t len, uint32_t autorunAddr, uint32_t ramStart,
              uint32_t ramEnd, const std::vector<BasicArea>& basicAreas) {
    Advice a;
    const uint32_t end = addr + static_cast<uint32_t>(len);  // one past the last byte
    const uint32_t entry = autorunAddr != 0 ? autorunAddr : addr;
    a.callNote = autorunAddr != 0 ? "auto-run address from the file header"
                                  : "start of the code -- assumes the entry point is its first byte";

    if (target == Target::PC1500) {
        a.callCommand = "CALL " + hex(entry);
        const uint32_t basicMin = ramStart + kReserve;
        if (addr >= basicMin && end < ramEnd) {
            a.newCommand = "NEW " + hex(end);
            a.newNote = "BASIC then starts right after the code, at " + hex(end) + ".";
        } else if (addr >= ramStart && addr < basicMin) {
            a.newNote = "The code starts inside the " + std::to_string(kReserve) + "-byte BASIC reserve (" +
                        hex(ramStart) + "-" + hex(basicMin - 1) + "), which NEW can't protect. Load it at " +
                        hex(basicMin) + " or higher.";
        } else if (addr < ramEnd && end > ramStart) {
            a.newNote = "The code reaches the end of user RAM (" + hex(ramEnd - 1) +
                        "), so NEW can't leave room for BASIC after it.";
        } else {
            a.newNote = "The code is outside the BASIC RAM area (" + hex(ramStart) + "-" + hex(ramEnd - 1) +
                        "), so BASIC doesn't use it and no NEW is needed.";
        }
        return a;
    }

    // Slot 2's window is global bank 2 (PC-1600-Memory-Architecture.md §4);
    // S0 and slot 1 are reached from bank 0. Checked in the emulator:
    // `CALL #2,&80C5` runs code loaded at $80C5 in a slot 2 CE-1600M.
    a.callCommand = slot == Slot::S2 ? "CALL #2," + hex(entry) : "CALL " + hex(entry);

    std::string workAreaWarning;
    if (slot == Slot::S0 && end > kPc1600WorkArea) {
        if (addr < kPc1600Wake)
            workAreaWarning = " Warning: the code reaches into the system work area (&F000-&FEFF) and will be "
                              "overwritten.";
        else if (addr < kPc1600FreeTop)
            workAreaWarning = " Warning: &FF00-&FF3F holds the WAKE$ strings -- a long WAKE$ and the code "
                              "overwrite each other. Load it at &FF40 or higher.";
        else
            workAreaWarning = " &FF40-&FFFF is unused by the system but officially reserved for the CE-1F01A "
                              "bar-code reader software, so don't use both.";
    }

    // Which run of the BASIC program area (if any) the code lands in.
    int hit = -1;
    for (size_t i = 0; i < basicAreas.size(); i++) {
        const BasicArea& area = basicAreas[i];
        if (sameMemory(area, slot, addr) && addr <= area.top && end > area.windowBase) {
            hit = static_cast<int>(i);
            break;
        }
    }
    if (basicAreas.empty()) {
        a.newNote = "The BASIC program area couldn't be read, so no NEW can be worked out." + workAreaWarning;
    } else if (hit == 0) {
        const BasicArea& first = basicAreas.front();
        a.newCommand = "NEW \"S0:\"," + hex(end - first.windowBase);
        a.newNote = "BASIC's program area starts" + std::string(areaWhere(first.slot)) + "; this reserves " +
                    hex(first.windowBase + kReserve) + "-" + hex(end - 1) + " there for machine code.";
        if (addr < first.windowBase + kReserve)
            a.newNote += " Warning: the code starts below " + hex(first.windowBase + kReserve) +
                         ", inside the area the system keeps for itself -- load it at " +
                         hex(first.windowBase + kReserve) + " or higher.";
        a.newNote += workAreaWarning;
    } else if (hit > 0) {
        const BasicArea& first = basicAreas.front();
        a.newNote = "BASIC's program area starts at " + hex(first.windowBase + kReserve) + areaWhere(first.slot) +
                    " and continues into this memory, so a long BASIC program can overwrite the code. NEW "
                    "\"S0:\" only reserves from the start of that area -- load the code at " +
                    hex(first.windowBase + kReserve) + areaWhere(first.slot) + " to protect it." + workAreaWarning;
    } else {
        a.newNote = "BASIC doesn't use this memory, so the code needs no NEW." + workAreaWarning;
    }
    return a;
}

bool parseHexAddress(const std::string& text, uint32_t* out) {
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
