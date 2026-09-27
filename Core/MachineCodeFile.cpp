#include "MachineCodeFile.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "PC1600/PC1600MachineImage.hpp"
#include "PC1600/PC1600ProgramPlacement.hpp"

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
        f.loadAddr = img.loadAddr;
        f.autorunAddr = img.autorunAddr;
        f.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(img.headerSize), bytes.end());
        if (img.headerPayloadLen != f.payload.size()) {
            char b[160];
            std::snprintf(b, sizeof(b), "The PC-1600 header says %u bytes of code, but %zu follow it.",
                          img.headerPayloadLen, f.payload.size());
            f.error = b;
            f.lengthMismatch = true;
            return f;
        }
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
        f.loadAddr = be16(bytes, 0x15);
        f.autorunAddr = be16(bytes, 0x19);
        if (f.autorunAddr == 0xFFFF) f.autorunAddr = 0;  // &FFFF = no auto-start, same as 0
        f.payload.assign(bytes.begin() + kCe158HeaderSize, bytes.end());
        if (len != f.payload.size()) {
            char b[160];
            std::snprintf(b, sizeof(b), "The CE-158 header says %zu bytes of code, but %zu follow it.", len,
                          f.payload.size());
            f.error = b;
            f.lengthMismatch = true;
            return f;
        }
        f.ok = true;
        return f;
    }
    f.payload = bytes;
    f.ok = true;
    return f;
}

bool pc1600TargetFor(uint32_t busAddr, size_t len, const PC1600State& state, Slot* slot, std::string* why, Cpu cpu) {
    // Messages name addresses the way the user gave them: LH5803 addresses
    // in MODE 1 (LH5801 code), Z-80 addresses otherwise.
    const bool lh = cpu == Cpu::LH5803;
    auto show = [lh](uint32_t z80) {
        return lh ? hex(pc1600::z80ToLh5803(static_cast<uint16_t>(z80))) : hex(z80);
    };
    const std::string at = (lh ? "LH5803 " : "") + show(busAddr);
    const uint64_t end = static_cast<uint64_t>(busAddr) + len;  // one past the last byte
    if (busAddr >= kPc1600S0Base) {
        if (end > 0x10000) {
            if (why) *why = at + " + " + std::to_string(len) + " bytes runs past " + show(0xFFFF) + ".";
            return false;
        }
        *slot = Slot::S0;
        return true;
    }
    if (busAddr < kPc1600SlotBase) {
        if (why) *why = at + " is ROM -- machine code goes into RAM, from " + show(pc1600DefaultAddress(state)) + ".";
        return false;
    }
    if (end > kPc1600S0Base) {
        if (why)
            *why = at + " + " + std::to_string(len) + " bytes crosses " + show(kPc1600S0Base) +
                   " -- a memory module's window is " + show(kPc1600SlotBase) + "-" + show(kPc1600S0Base - 1) + ".";
        return false;
    }
    // $8000-$BFFF: the module behind the selected program area.
    if (state.title == 1 || state.title == 2) {
        if (busAddr < state.titleBase) {
            if (why) *why = at + " is below the slot " + std::to_string(state.title) + " module, which starts at " +
                            show(state.titleBase) + ".";
            return false;
        }
        *slot = state.title == 1 ? Slot::S1 : Slot::S2;
        return true;
    }
    // TITLE S0: the module folded into S0 as extension memory (the area's
    // first run), if any.
    if (state.basicAreas.empty()) {
        if (why) *why = "BASIC's program area couldn't be read, so " + at + " can't be placed.";
        return false;
    }
    const BasicArea& first = state.basicAreas.front();
    if (first.slot == 0) {
        if (why)
            *why = at + " would be in a memory module, but no module is part of the selected program area -- "
                   "that is internal RAM, from " + show(first.windowBase + kReserve) +
                   ". (A program module has to be selected first: TITLE\"Sx:\".)";
        return false;
    }
    if (busAddr < first.windowBase) {
        if (why) *why = at + " is below the slot " + std::to_string(first.slot) + " module, which starts at " +
                        show(first.windowBase) + ".";
        return false;
    }
    *slot = first.slot == 1 ? Slot::S1 : Slot::S2;
    return true;
}

uint32_t pc1600DefaultAddress(const PC1600State& state) {
    if ((state.title == 1 || state.title == 2) && state.titleStart != 0) return state.titleStart;
    return (state.basicAreas.empty() ? kPc1600S0Base : state.basicAreas.front().windowBase) + kReserve;
}

std::string pc1600WorkAreaWarning(uint32_t busAddr, size_t len, Cpu cpu) {
    const uint64_t end = static_cast<uint64_t>(busAddr) + len;
    if (end <= kPc1600WorkArea || busAddr > 0xFFFF) return {};
    const bool lh = cpu == Cpu::LH5803;
    auto show = [lh](uint32_t z80) {
        return lh ? hex(pc1600::z80ToLh5803(static_cast<uint16_t>(z80))) : hex(z80);
    };
    if (busAddr < kPc1600Wake) {
        if (lh)
            return "Warning: LH5803 " + show(kPc1600WorkArea) + "-" + show(kPc1600Wake - 1) +
                   " is the PC-1600's system work area (Z-80 &F000-&FEFF): whatever the system uses there and the "
                   "code overwrite each other. The PC-1500A's machine-code area &7C01 lies there -- one reason the "
                   "PC-1600 is compatible with the PC-1500 only.";
        return "Warning: the code lies in the system work area (&F000-&FEFF): whatever the system uses there and the "
               "code overwrite each other.";
    }
    if (busAddr < kPc1600FreeTop)
        return "Warning: " + show(kPc1600Wake) + "-" + show(kPc1600FreeTop - 1) +
               " holds the WAKE$ strings -- a long WAKE$ and the code overwrite each other. Load it at " +
               show(kPc1600FreeTop) + " or higher.";
    return show(kPc1600FreeTop) + "-" + show(0xFFFF) +
           " is unused by the system but officially reserved for the CE-1F01A bar-code reader software, so don't use "
           "both.";
}

std::string headerMismatch(Target target, const File& file, bool mode1) {
    if (target == Target::PC1600 && file.header == File::Header::CE158 && !mode1)
        return "This file has a CE-158 (PC-1500) header: PC-1500 machine code, which the PC-1600 takes in MODE 1 "
               "only. Type MODE1 first.";
    if (target == Target::PC1500 && file.header == File::Header::PC1600)
        return "This file has a PC-1600 header and can't be loaded into a PC-1500.";
    return {};
}

LoadPlan planLoad(const File& file, const LoadOptions& o, const PC1600State& state) {
    LoadPlan p;
    auto refuse = [&p](LoadError e, std::string detail = {}) {
        p.error = e;
        p.detail = std::move(detail);
        return p;
    };
    const bool pc1600 = o.target == Target::PC1600;
    if (!file.ok && !(file.lengthMismatch && o.acceptLengthMismatch)) return refuse(LoadError::BadFile, file.error);
    const std::string mismatch = headerMismatch(o.target, file, state.mode1);
    if (!mismatch.empty()) return refuse(LoadError::HeaderMismatch, mismatch);
    // The CPU: the header's, else the caller's, else the MODE's.
    if (pc1600) {
        if (file.header == File::Header::PC1600) p.cpu = Cpu::Z80;
        else if (file.header == File::Header::CE158) p.cpu = Cpu::LH5803;
        else if (o.hasCpu) p.cpu = o.cpu;
        else p.cpu = state.mode1 ? Cpu::LH5803 : Cpu::Z80;
    }
    if (file.payload.empty() && !o.hasLength) return refuse(LoadError::Empty);
    if (file.header == File::Header::None && !o.hasAddress) {
        if (pc1600) {
            const uint32_t def = pc1600DefaultAddress(state);
            p.defaultAddr = p.cpu == Cpu::LH5803 ? pc1600::z80ToLh5803(static_cast<uint16_t>(def)) : def;
        }
        return refuse(LoadError::NeedsAddress);
    }
    p.addr = o.hasAddress ? o.address : file.loadAddr;
    p.len = o.hasLength ? o.length : file.payload.size();
    if (p.len == 0) return refuse(LoadError::Empty);
    if (p.len > file.payload.size()) return refuse(LoadError::LengthExceeds);
    if (o.checkRange && p.addr > 0xFFFF) return refuse(LoadError::OutsideBank0);
    p.busAddr = p.addr;
    if (pc1600 && p.cpu == Cpu::LH5803) {
        if (p.addr + p.len > 0x8000) return refuse(LoadError::LhRange);
        p.busAddr = pc1600::lh5803ToZ80(static_cast<uint16_t>(p.addr));
    }
    if (pc1600) {
        std::string why;
        if (!pc1600TargetFor(p.busAddr, p.len, state, &p.slot, &why, p.cpu)) return refuse(LoadError::NoSlot, why);
    }
    if (o.checkRange && static_cast<uint64_t>(p.busAddr) + p.len > 0x10000) return refuse(LoadError::PastEnd);
    return p;
}

Plan plan(Target target, const File& file, const PC1600State& state) {
    // Load Machine Code…'s rules: the header decides, the machine's MODE and
    // TITLE pick the rest; a headerless file asks for its address.
    LoadOptions o;
    o.target = target;
    const LoadPlan lp = planLoad(file, o, state);
    Plan p;
    p.slot = lp.slot;
    p.cpu = lp.cpu;
    p.busAddr = lp.busAddr;
    switch (lp.error) {
        case LoadError::None: break;
        case LoadError::NeedsAddress:
            p.needsAddress = true;
            p.defaultAddr = lp.defaultAddr;
            break;
        case LoadError::Empty: p.error = "The file contains no code."; break;
        case LoadError::OutsideBank0:
            p.error = "The header's load address " + hex(file.loadAddr) + " is outside bank 0; banked loads are not supported.";
            break;
        case LoadError::LhRange:
            p.error = "The header's load address " + hex(file.loadAddr) + " + " + std::to_string(file.payload.size()) +
                      " bytes is outside the LH5803's RAM (&0000-&7FFF).";
            break;
        case LoadError::NoSlot: p.error = "The header's load address doesn't fit: " + lp.detail; break;
        case LoadError::PastEnd:
            p.error = "The code (" + std::to_string(file.payload.size()) + " bytes at " + hex(file.loadAddr) + ") runs past &FFFF.";
            break;
        default: p.error = lp.detail; break; // BadFile, HeaderMismatch
    }
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
              uint32_t ramEnd, const PC1600State& state, Cpu cpu) {
    Advice a;
    const uint32_t entry = autorunAddr != 0 ? autorunAddr : addr;
    a.callNote = autorunAddr != 0 ? "auto-run address from the file header"
                                  : "start of the code -- assumes the entry point is its first byte";

    if (target == Target::PC1500) {
        const uint32_t end = addr + static_cast<uint32_t>(len);  // one past the last byte
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

    // PC-1600. LH5801 code is started from BASIC with XCALL (an LH5803
    // address); Z-80 code with CALL. Slot 2's window is global bank 2
    // (PC-1600-Memory-Architecture.md §4); S0 and slot 1 are reached from
    // bank 0. Checked in the emulator: `CALL #2,&80C5` runs code loaded at
    // $80C5 in a slot 2 CE-1600M.
    if (cpu == Cpu::LH5803) {
        a.callCommand = "XCALL " + hex(entry);
        a.callNote += " (an LH5803 address: the code is LH5801 code)";
    } else {
        a.callCommand = slot == Slot::S2 ? "CALL #2," + hex(entry) : "CALL " + hex(entry);
    }
    // The rest works on Z-80 addresses.
    const uint32_t bus = cpu == Cpu::LH5803 ? pc1600::lh5803ToZ80(static_cast<uint16_t>(addr)) : addr;
    const uint32_t end = bus + static_cast<uint32_t>(len);
    auto inCpu = [cpu](uint32_t z80) {
        return cpu == Cpu::LH5803 ? pc1600::z80ToLh5803(static_cast<uint16_t>(z80)) : z80;
    };

    std::string workAreaWarning;
    if (slot == Slot::S0) {
        const std::string w = pc1600WorkAreaWarning(bus, len, cpu);
        if (!w.empty()) workAreaWarning = " " + w;
    }

    // Code in the selected S1 / S2 program module: NEW "Sn:",size reserves
    // it at the start of that module (size counted from the module base).
    if ((slot == Slot::S1 && state.title == 1) || (slot == Slot::S2 && state.title == 2)) {
        const uint32_t start = state.titleBase + kReserve;
        a.newCommand = "NEW \"S" + std::to_string(state.title) + ":\"," + hex(end - state.titleBase);
        a.newNote = "BASIC's selected program area is the slot " + std::to_string(state.title) +
                    " program module; this reserves " + hex(inCpu(start)) + "-" + hex(inCpu(end - 1)) +
                    " there for machine code.";
        if (bus < start)
            a.newNote += " Warning: the code starts below " + hex(inCpu(start)) +
                         ", inside the module header and reserve area.";
        return a;
    }

    // Which run of the S0 program area (if any) the code lands in.
    int hit = -1;
    for (size_t i = 0; i < state.basicAreas.size(); i++) {
        const BasicArea& area = state.basicAreas[i];
        if (sameMemory(area, slot, bus) && bus <= area.top && end > area.windowBase) {
            hit = static_cast<int>(i);
            break;
        }
    }
    if (state.basicAreas.empty()) {
        a.newNote = "The BASIC program area couldn't be read, so no NEW can be worked out." + workAreaWarning;
        return a;
    }
    const BasicArea& first = state.basicAreas.front();
    const uint32_t areaStart = first.windowBase + kReserve;
    if (state.title != 0) {
        a.newNote = "BASIC's selected program area is the slot " + std::to_string(state.title) +
                    " program module, not this memory, so the program there doesn't grow into the code; variables "
                    "in internal RAM still can." + workAreaWarning;
        return a;
    }
    if (hit >= 0 && state.mode1) {
        // MODE 1: the PC-1500 way, NEW <address> -- BASIC starts right after
        // the code (rom3b 4351H: at least the S0 start + &C5, below the
        // variable area; an LH5803 address).
        if (bus >= areaStart && (state.ramEnd == 0 || end < state.ramEnd)) {
            a.newCommand = "NEW " + hex(pc1600::z80ToLh5803(static_cast<uint16_t>(end)));
            a.newNote = "MODE 1 moves BASIC's start the PC-1500 way: BASIC then starts right after the code, at LH5803 " +
                        hex(pc1600::z80ToLh5803(static_cast<uint16_t>(end))) + ".";
        } else {
            a.newNote = "The code starts below " + hex(inCpu(areaStart)) +
                        ", inside the area the system keeps for itself, so NEW can't protect it -- load it at " +
                        hex(inCpu(areaStart)) + " or higher.";
        }
        a.newNote += workAreaWarning;
        return a;
    }
    if (hit == 0) {
        a.newCommand = "NEW \"S0:\"," + hex(end - first.windowBase);
        a.newNote = "BASIC's program area starts" + std::string(areaWhere(first.slot)) + "; this reserves " +
                    hex(areaStart) + "-" + hex(end - 1) + " there for machine code.";
        if (bus < areaStart)
            a.newNote += " Warning: the code starts below " + hex(areaStart) +
                         ", inside the area the system keeps for itself -- load it at " + hex(areaStart) +
                         " or higher.";
        // NEW "S0:" can only reserve from the START of the area, so code
        // parked high costs BASIC everything underneath it as well. Say
        // what that costs -- for code deliberately placed in the middle of
        // the user area, no NEW at all is usually the better trade.
        if (bus > areaStart) {
            const uint32_t below = bus - areaStart;
            a.newNote += " Note: only " + std::to_string(len) + " of those bytes are the code -- the " +
                         std::to_string(below) + " bytes below it (" + hex(areaStart) + "-" + hex(bus - 1) +
                         ") are reserved along with it and lost to BASIC. If the code was placed this high on "
                         "purpose, skip the NEW and keep the program and its variables from growing into it "
                         "instead.";
        }
        a.newNote += workAreaWarning;
    } else if (hit > 0) {
        a.newNote = "BASIC's program area starts at " + hex(inCpu(areaStart)) + areaWhere(first.slot) +
                    " and continues into this memory, so a long BASIC program can overwrite the code. NEW only "
                    "reserves from the start of that area -- load the code at " + hex(inCpu(areaStart)) +
                    areaWhere(first.slot) + " to protect it." + workAreaWarning;
    } else {
        // No run of the program area covers the code. In internal RAM that
        // does not mean BASIC never comes here: &C0C5-&EFFF is ONE user
        // area (TRM §6.1) shared by the machine/BASIC program growing up
        // from the bottom and the variables growing down from &EFFF, and
        // the program area's top is a live ceiling (the variable pointer,
        // $F899). Code parked above it is free right now but sits in the
        // variables' path -- and a NEW long enough to protect it would
        // reserve everything below it too, which is why a program is
        // deliberately placed up here without one.
        const BasicArea* internal = nullptr;
        for (const BasicArea& area : state.basicAreas)
            if (area.slot == 0) internal = &area;
        if (slot == Slot::S0 && internal && bus > internal->top && end <= kPc1600WorkArea) {
            a.newNote = "Nothing uses this memory right now: the BASIC program area reaches up to " +
                        hex(inCpu(internal->top)) + ", and " + hex(inCpu(internal->top + 1)) + "-" +
                        hex(inCpu(kPc1600WorkArea - 1)) +
                        " above it is the variable area, which grows downwards. No NEW is needed -- and one long "
                        "enough to reach here would reserve everything below it as well, giving up most of the free "
                        "memory -- but variables growing down can still overwrite the code.";
        } else {
            a.newNote = "BASIC doesn't use this memory, so the code needs no NEW.";
        }
        a.newNote += workAreaWarning;
    }
    return a;
}

}  // namespace machinecode
