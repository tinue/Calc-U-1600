#include "PC1600Inspector.hpp"

#include <algorithm>

#include "../../PC1600/PC1600BusDrive.hpp"
#include "../../PC1600/PC1600Machine.hpp"
#include "../../PC1600/PC1600ProgramPlacement.hpp"
#include "../BasicPointerTable.hpp"
#include "InspectCommon.hpp"
#include "TextTable.hpp"

namespace inspect {
namespace {

using pc1600::lh5803ToZ80;
using R = TextTable::Align;

constexpr uint16_t kWorkArea = 0xF000;
constexpr uint32_t kBank = 0x4000;

// ── One snapshot of what every view reads ────────────────────────────────

struct SlotInfo {
    const ExpansionCard* card = nullptr;
    std::string name;
    std::vector<uint8_t> image;  // every bank, ascending (debugImage())
    std::vector<CardMemory> memories;
    int bankCount = -1;          // the card's own banks (-1 none)
    int bank = -1;               // its latched bank now
    pc1600::SlotGeometry geom;
    bool portBanked() const {
        for (const CardMemory& c : memories)
            if (c.portBanked) return true;
        return false;
    }
    std::string label(int slot) const {
        return card ? fmt("Slot %d — %s", slot, name.empty() ? "module" : name.c_str()) : fmt("Slot %d", slot);
    }
};

struct Ctx {
    const PC1600Machine& m;
    const uint8_t* ram;
    PC1600Machine::DebugBankState bs;
    SlotInfo slot[3];  // [1], [2]

    explicit Ctx(const PC1600Machine& machine)
        : m(machine), ram(machine.memory().internalRam()), bs(machine.inspectBankState()) {
        for (int s = 1; s <= 2; ++s) {
            SlotInfo& i = slot[s];
            i.card = m.memory().slotCard(s);
            if (!i.card) continue;
            i.name = i.card->moduleName();
            i.image = i.card->debugImage();
            i.memories = i.card->debugMemories();
            i.bankCount = i.card->debugBankCount();
            i.bank = i.card->debugCurrentBank();
            i.geom.present = !i.image.empty();
            i.geom.imageSize = uint32_t(i.image.size());
            i.geom.bankSize = i.bankCount > 0 ? i.geom.imageSize / uint32_t(i.bankCount) : i.geom.imageSize;
        }
    }

    /// A work-area / internal-RAM byte (C000H up), read in place; below
    /// that, through the current banks.
    uint8_t wa(uint16_t a) const { return a >= 0xC000 ? ram[a - 0xC000] : m.memory().peek(a); }
    uint16_t be(uint16_t a) const { return uint16_t(wa(a) << 8 | wa(uint16_t(a + 1))); }
    uint16_t le(uint16_t a) const { return uint16_t(wa(uint16_t(a + 1)) << 8 | wa(a)); }

    pc1600::PlacementInput placementInput() const {
        pc1600::PlacementInput in;
        in.peek = [this](uint16_t a) { return wa(a); };
        in.slot1 = slot[1].geom;
        in.slot2 = slot[2].geom;
        return in;
    }

    /// A byte of global page-C `bank` (0-3) at Z-80 `addr`, as the CPU would
    /// read it with that bank mapped; internal RAM for C000H up.
    uint8_t read(int bank, uint16_t addr) const {
        if (addr >= 0xC000) return ram[addr - 0xC000];
        uint8_t v = 0xFF;
        m.memory().slotBusRead(bank, addr, v);
        return v;
    }
};

// ── Pointers ─────────────────────────────────────────────────────────────

enum class V : uint8_t {
    Byte,
    AddrBE,     // a BASIC pointer as the ROM keeps it: LH5803 view, high byte first
    WordBE,     // a big-endian number (a line number)
    WordLE,     // a Z-80 word
    Z80Bank,    // PRGSTA/PRGENDA: Z-80 address (low first), then the ADTBL index
};

using Decode = std::string (*)(uint8_t);

struct Ptr {
    const char* name;
    uint16_t addr;
    V value;
    const char* note;
    Decode decode = nullptr;
};

struct Group {
    const char* title;
    std::vector<Ptr> ptrs;
};

std::string decodeTitle(uint8_t v) { return v <= 2 ? fmt("S%u", v) : "?"; }
std::string decodeBmode(uint8_t v) {
    std::string s = v & 0x40 ? "MODE 1" : "MODE 0";
    if (v & 0x02) s += ", BREAK OFF";
    if (v & 0x01) s += ", in error routine";
    return s;
}
std::string decodeSymb1(uint8_t v) { return bits(v, {{6, "RUN"}, {5, "PRO"}, {4, "RESERVE"}}); }
std::string decodePort3d(uint8_t v) { return v & 0x04 ? "bank 3 BASIC ROM" : "bank 3b (hidden) BASIC ROM"; }
std::string decodeRstCause(uint8_t v) {
    return bits(v, {{0, "ALL RESET"}, {1, "RESET"}, {2, "ext reset"}, {4, "ON"}, {5, "ext power-on"},
                    {6, "WAKE$(0)"}, {7, "WAKE$(1)"}});
}
std::string decodeBintreq(uint8_t v) { return bits(v, {{7, "WAKE$"}, {6, "ON TIME$"}, {5, "ALARM$"}}); }
std::string decodeRamPart(uint8_t v) { return sizeLabel(uint32_t(v) * 2048); }
std::string decodeAdtbl(uint8_t v) {
    if (v == 0) return "unused";
    return fmt("slot %u, bank %u%s", v & 0x0F, (v >> 4) & 3, v & 0x80 ? ", leading bank" : "");
}
std::string decodeMtb(uint8_t v) {
    if (v == 0xFE) return "folded into S0";
    if (v == 0xFF) return "none";
    return v >= 1 && v <= 5 ? fmt("ADTBL+%u", v) : "?";
}

// Addresses and names from the ROM disassembly's symbol tables and
// Ref/PC-1600/PC-1600-Work-Area-Map.md (§3.2 display, §3.5 BASIC, §3.9
// timer, §3.12 logical banks, §3.13/§3.15 ports, §3.18 RAM disk, §4 ADTBL).
const std::vector<Group>& pc1600Pointers() {
    static const std::vector<Group> groups = {
        {"BASIC program",
         {{"BASPRG_ST", 0xF865, V::AddrBE, "S0 program start (ADTBL index F02BH)"},
          {"BASPRG_END", 0xF867, V::AddrBE, "S0 program end, the FFH mark (ADTBL index F02CH)"},
          {"EDITHEAD", 0xF869, V::AddrBE, "Edit / merge head"},
          {"PRGSTA", 0xFE3C, V::Z80Bank, "Start of the TITLE area (PRGADR 02F4H)"},
          {"PRGENDA", 0xFE3F, V::Z80Bank, "End of the TITLE area"},
          {"PROCPTR", 0xFE00, V::WordLE, "Interpreter: the next program byte (Z-80)"},
          {"CURTOP", 0xF89E, V::AddrBE, "Block of the current line"},
          {"LBCUR", 0xF1C1, V::Byte, "ADTBL index of the current line"},
          {"CURRENT_LINE", 0xF89C, V::WordBE, "Current line number"},
          {"SRCHLINE", 0xF8A8, V::WordBE, "Line after a SEARCH hit / last entered"},
          {"CONTADR", 0xF0B9, V::WordLE, "CONT / restart address (Z-80)"},
          {"TRON", 0xF88D, V::Byte, "TRON", decodeOnOff},
          {"TRONMODE", 0xF88E, V::Byte, "TRON single-step state (1-4)"}}},
        {"Errors and BREAK",
         {{"ERL", 0xF89B, V::Byte, "Last error number"},
          {"BRKADR", 0xF8AC, V::AddrBE, "BREAK address"},
          {"BRKLINE", 0xF8AE, V::WordBE, "BREAK line"},
          {"ERRADR", 0xF8B2, V::AddrBE, "Error address"},
          {"ERRLINE", 0xF8B4, V::WordBE, "Error line"},
          {"ONERADR", 0xF8B8, V::AddrBE, "ON ERROR GOTO target (bit 15 set: none)"},
          {"ONERLINE", 0xF8BA, V::WordBE, "ON ERROR line"}}},
        {"Variables and stacks",
         {{"RAM_END", 0xF864, V::Byte, "Page of the S0 user-area top (MEM, VARPTR reset)"},
          {"VARPTR", 0xF899, V::AddrBE, "Start of the variables (they grow down)"},
          {"FORPTR", 0xF890, V::Byte, "FOR stack pointer (low byte)"},
          {"GOSUBPTR", 0xF891, V::Byte, "GOSUB stack pointer (low byte)"},
          {"STRBUFP", 0xF894, V::Byte, "String buffer pointer (low byte, FB10H-FB5FH)"},
          {"LOCKF", 0xF9FF, V::Byte, "LOCK / UNLOCK state"}}},
        {"Program areas",
         {{"TITLE", 0xF1D5, V::Byte, "Selected program area", decodeTitle},
          {"S0DESC", 0xF029, V::Byte, "S0 base page (b6-0); ML area from base:C5H"},
          {"S0MTB", 0xF02A, V::Byte, "First ADTBL index of S0"},
          {"PRGBANKS", 0xF02B, V::Byte, "ADTBL index of the S0 program start"},
          {"PRGBANKE", 0xF02C, V::Byte, "ADTBL index of the S0 program end"},
          {"ADTBL+1", 0xF1D6, V::Byte, "Bank list", decodeAdtbl},
          {"ADTBL+2", 0xF1D7, V::Byte, "", decodeAdtbl},
          {"ADTBL+3", 0xF1D8, V::Byte, "", decodeAdtbl},
          {"ADTBL+4", 0xF1D9, V::Byte, "", decodeAdtbl},
          {"ADTBL+5", 0xF1DA, V::Byte, "", decodeAdtbl},
          {"S1MTB", 0xF016, V::Byte, "First ADTBL index of S1", decodeMtb},
          {"S1MBB", 0xF018, V::Byte, "Last ADTBL index of S1"},
          {"S2MTB", 0xF020, V::Byte, "First ADTBL index of S2", decodeMtb},
          {"S2MBB", 0xF022, V::Byte, "Last ADTBL index of S2"}}},
        {"Files and RAM disks",
         {{"FBNO", 0xF02D, V::Byte, "MAXFILES"},
          {"FBBP", 0xF04C, V::WordLE, "Communication-buffer start (PTRF)"},
          {"FCBPTR", 0xF04E, V::WordLE, "FCB / file-buffer start (PTRG)"},
          {"S1RAMSIZ", 0xF055, V::Byte, "Slot 1 RAM outside the RAM disk (2 KB units)", decodeRamPart},
          {"S2RAMSIZ", 0xF05B, V::Byte, "Slot 2 RAM outside the RAM disk (2 KB units)", decodeRamPart},
          {"DEVNAME", 0xFC16, V::Byte, "Device of the current file operation"}}},
        {"Mode and system",
         {{"BMODE", 0xF1BC, V::Byte, "", decodeBmode},
          {"SYMB1", 0xF64F, V::Byte, "Mode annunciators", decodeSymb1},
          {"PORT3D_M", 0xF07D, V::Byte, "Port 3DH mirror", decodePort3d},
          {"SLOTMAPM", 0xF08D, V::Byte, "Port 3CH mirror (SLOTMAP)"},
          {"RSTCAUSE", 0xFA1B, V::Byte, "Last reset / power-on cause", decodeRstCause},
          {"BINTREQ", 0xF127, V::Byte, "Pending BASIC interrupts", decodeBintreq},
          {"BINTEN", 0xF12A, V::Byte, "Enabled sub-CPU interrupts (SWMSK)"},
          {"OPNDV", 0xF9D1, V::Byte, "PC-1500 peripheral (OPN)"}}},
        {"Display and keyboard",
         {{"DSPLPTR", 0xF05C, V::Byte, "LCD display start line"},
          {"LCDWK1", 0xF05D, V::Byte, "LCD work 1 (charset / cursor flags)"},
          // F05F = row, F060 = column, as the ROM's own labels have it; the
          // TRM swaps the two (Work-Area-Map §3.2).
          {"CRSRY", 0xF05F, V::Byte, "Cursor row"},
          {"CRSRX", 0xF060, V::Byte, "Cursor column"},
          {"KEYWK1", 0xF079, V::Byte, "Key work 1 (click / repeat flags)"}}},
    };
    return groups;
}

std::string pointerValue(const Ctx& c, const Ptr& p) {
    switch (p.value) {
        case V::Byte: return fmt("%02X", c.wa(p.addr));
        case V::AddrBE: {
            const uint16_t w = c.be(p.addr);
            return w & 0x8000 ? fmt("%04X (none)", w) : fmt("%04X → Z80 %04X", w, lh5803ToZ80(w));
        }
        case V::WordBE: return fmt("%04X (%u)", c.be(p.addr), c.be(p.addr));
        case V::WordLE: return fmt("%04X", c.le(p.addr));
        case V::Z80Bank: return fmt("%04X [%u]", c.le(p.addr), c.wa(uint16_t(p.addr + 2)));
    }
    return "?";
}

/// Ranges of the S0 BASIC area: where the program, the free gap and the
/// variables lie, computed the ROM's way.
struct S0Figures {
    uint16_t start = 0, end = 0;   // Z-80 addresses of the first byte / the FFH mark
    int startIndex = 0, endIndex = 0;
    uint16_t varPtr = 0, ramEnd = 0;  // Z-80; ramEnd = RAM_END:00
    uint16_t mlStart = 0;             // base:C5H, Z-80
    int programBytes = 0;             // first byte up to and with the FFH mark
};

S0Figures s0Figures(const Ctx& c) {
    S0Figures f;
    f.start = lh5803ToZ80(c.be(0xF865));
    f.end = lh5803ToZ80(c.be(0xF867));
    f.startIndex = c.wa(0xF02B);
    f.endIndex = c.wa(0xF02C);
    f.varPtr = lh5803ToZ80(c.be(0xF899));
    f.ramEnd = lh5803ToZ80(uint16_t(c.wa(0xF864) << 8));
    f.mlStart = lh5803ToZ80(uint16_t((c.wa(0xF029) & 0x7F) << 8 | 0xC5));
    // Work-Area-Map §3.5: the ADTBL index steps 4000H per bank.
    f.programBytes = int(f.end) - int(f.start) + (f.endIndex - f.startIndex) * int(kBank) + 1;
    return f;
}

std::vector<std::string> pointersView(const Ctx& c) {
    TextTable t({"Name", "Addr", "Value", "Meaning"});
    for (const Group& g : pc1600Pointers()) {
        t.addSpan(g.title);
        for (const Ptr& p : g.ptrs) {
            std::string note = p.note;
            if (p.decode) {
                const std::string d = p.decode(c.wa(p.addr));
                note = note.empty() ? d : note + ": " + d;
            }
            t.addRow({p.name, fmt("%04X", p.addr), pointerValue(c, p), note});
        }
    }

    t.addSpan("Live");
    const uint16_t sp = c.m.sc7852().sp();
    t.addRow({"SP", "", fmt("%04X", sp),
              sp >= 0xF500 && sp < 0xF600 ? "SC7852 stack pointer (Z-80 stack F500H-F5FFH)"
                                          : "SC7852 stack pointer (outside F500H-F5FFH)"});
    t.addRow({"Port 31H", "", fmt("%02X", c.bs.port31),
              fmt("banks A %u, B %u, C %u, D %u", c.bs.pageABank, c.bs.pageBBank, c.bs.pageCBank, c.bs.pageDBank)});

    const S0Figures f = s0Figures(c);
    const CoreDebug::PC1600ProgramAreas areas = CoreDebug::readPC1600ProgramAreas([&](uint16_t a) { return c.wa(a); });
    t.addSpan("Computed");
    t.addRow({"MEM", "", fmt("%d", areas.memS0), "S0 free bytes: RAM_END:00 − (BASPRG_END + 1) + (5 − F02CH) × 4000H"});
    t.addRow({"Program", "", fmt("%d", f.programBytes), "S0 program, with the FFH mark"});
    const int ml = int(f.start) - int(f.mlStart);
    t.addRow({"ML reserve", "", fmt("%d", ml > 0 ? ml : 0),
              fmt("Machine-language area %04X up to BASPRG_ST (NEW \"S0:\",n)", f.mlStart)});
    const int vars = int(f.ramEnd) - int(f.varPtr);
    t.addRow({"Variables", "", fmt("%d", vars > 0 ? vars : 0), "RAM_END:00 − VARPTR"});
    for (int i = 0; i < 2; ++i) {
        const CoreDebug::PC1600SlotProgramArea& sl = areas.slot[i];
        t.addRow({fmt("S%d free", i + 1), fmt("%04X", pc1600::slotDescriptorAddress(i + 1)),
                  sl.programModule() ? fmt("%d", sl.freeBytes) : "—",
                  sl.programModule() ? fmt("STATUS %d", 259 + i) : "no program module"});
    }
    // Work-Area-Map §2.2: slice n runs from PTRn up to PTRn−1 (PTR0 = F000H);
    // a slice is reserved when the two differ.
    uint16_t prev = kWorkArea;
    for (int n = 1; n <= 16; ++n) {
        const uint16_t addr = uint16_t(0xF02E + 2 * n);
        const uint16_t p = c.le(addr);
        if (p != prev && p < prev) {
            const char* what = n == 15 ? "communication buffer" : n == 16 ? "FCB / file buffers" : "EXROM work area";
            t.addRow({fmt("PTR%c", n <= 9 ? char('0' + n) : char('A' + n - 10)), fmt("%04X", addr),
                      range(p, prev - 1u), fmt("%s, %s", what, sizeLabel(uint32_t(prev - p)).c_str())});
        }
        prev = p;
    }
    return t.render();
}

// ── Inventory ────────────────────────────────────────────────────────────

std::string banksCell(const CardMemory& mem) {
    if (mem.banks <= 1) return "—";
    std::string s = fmt("%u × %s", mem.banks, sizeLabel(mem.size / mem.banks).c_str());
    if (mem.portBanked) s += fmt(" (port %02XH)", mem.port);
    return s;
}

/// Where a 60-pin card answers, found by offering it cycles: the Z-80's
/// page B banks 4-7, the LH5803's 8000H-BFFFH ROM window at each PU/PV.
std::string busCardSeenAt(const SystemBusCard* card) {
    std::vector<std::string> where;
    std::string z80;
    for (uint8_t bank = 4; bank <= 7; ++bank) {
        const SystemBusPins pins = PC1600BusDrive::z80MemPins(0x4000, false, bank);
        uint8_t v;
        if (card->respondsToRead(pins, v)) z80 += (z80.empty() ? "" : "/") + fmt("%u", bank);
    }
    if (!z80.empty()) where.push_back("Z80 4000–7FFF (B bank " + z80 + ")");
    for (const std::string& w : romWindowSeenAt(*card, [](uint16_t a, bool pu, bool pv) {
             return PC1600BusDrive::lh5803Pins(a, false, false, pu, pv);
         }))
        where.push_back("LH5803 " + w);
    return where.empty() ? "I/O only" : joinDots(where);
}

std::vector<std::string> inventoryView(const Ctx& c) {
    TextTable t({"Memory", "Kind", "Size", "Banks", "Seen at"}, {R::Left, R::Left, R::Right, R::Left, R::Left});
    const PC1600Memory& mem = c.m.memory();
    auto rom = [](bool loaded) { return loaded ? std::string("ROM") : std::string("ROM, not loaded"); };
    t.addSpan("Built in");
    t.addRow({"System ROM", rom(mem.systemRomLoaded()), "32K", "—", "Z80 0000–3FFF (A) · 4000–7FFF (B bank 0)"});
    t.addRow({"BASIC ROM bank 3", rom(mem.bank3RomLoaded()), "16K", "—", "Z80 4000–7FFF (B bank 3, port 3DH b2=1)"});
    t.addRow({"BASIC ROM bank 3b", rom(mem.bank3bRomLoaded()), "16K", "—", "Z80 4000–7FFF (B bank 3, port 3DH b2=0)"});
    t.addRow({"ROM IV (bank 6)", rom(mem.bank6RomLoaded()), "16K", "—", "Z80 8000–BFFF (C bank 6)"});
    t.addRow({"LH5803 ROM", rom(c.m.lh5803Memory().romLoaded()), "16K", "—", "LH5803 C000–FFFF"});
    t.addRow({"Internal RAM", "RAM", "16K", "—", "Z80 C000–FFFF (D bank 0) · LH5803 4000–7FFF"});

    t.addSpan("Memory slots");
    for (int s = 1; s <= 2; ++s) {
        const SlotInfo& i = c.slot[s];
        if (!i.card) {
            t.addRow({i.label(s), "empty"});
            continue;
        }
        const std::string seen = s == 1 ? "Z80 8000–BFFF (C banks 0/1) · LH5803 0000–3FFF"
                                         : "Z80 8000–BFFF (C banks 2/3) · LH5803 0000–3FFF";
        if (i.memories.empty()) {
            t.addRow({i.label(s), "I/O only", "", "", seen});
            continue;
        }
        for (size_t k = 0; k < i.memories.size(); ++k) {
            const CardMemory& cm = i.memories[k];
            const std::string name = k == 0 ? i.label(s) : "  " + cm.name;
            t.addRow({name, kindName(cm.kind), sizeLabel(cm.size), banksCell(cm), k == 0 ? seen : ""});
        }
    }

    t.addSpan("60-pin bus");
    const auto& cards = mem.systemBus().chain();
    if (cards.empty()) t.addRow({"(nothing attached)"});
    for (const SystemBusCard* card : cards) {
        const std::string name = cardName(*card);
        const std::vector<CardMemory> mems = card->debugMemories();
        if (mems.empty()) {
            t.addRow({name, "I/O only"});
            continue;
        }
        for (size_t k = 0; k < mems.size(); ++k)
            t.addRow({k == 0 ? name : "  " + mems[k].name, kindName(mems[k].kind), sizeLabel(mems[k].size),
                      banksCell(mems[k]), k == 0 ? busCardSeenAt(card) : ""});
    }
    return t.render();
}

// ── Z80 view ─────────────────────────────────────────────────────────────

using PT = PC1600Machine::PageTarget;

std::string slotKind(const SlotInfo& i) { return i.memories.empty() ? "I/O" : kindName(i.memories[0].kind); }

std::vector<std::string> z80View(const Ctx& c) {
    const auto& bs = c.bs;
    TextTable t({"Page", "Z80", "Bank", "Source", "Kind"});
    const char* pages = "ABCD";
    const uint8_t bank[4] = {bs.pageABank, bs.pageBBank, bs.pageCBank, bs.pageDBank};
    for (int p = 0; p < 4; ++p) {
        std::string src, kind = "ROM";
        // Which half of a slot module this page shows (PVOUT / A14 of the
        // module): page C's bank number, or the SLOTMAP redirect's.
        auto slotSrc = [&](int s, bool high) {
            const SlotInfo& i = c.slot[s];
            std::string x = i.label(s) + (high ? ", upper 16K" : ", lower 16K");
            if (s == 2 && i.portBanked()) x += fmt(", vertical bank %d", i.bank);
            kind = slotKind(i);
            return x;
        };
        switch (bs.target[p]) {
            case PT::OpenBus: src = "open bus"; kind = "—"; break;
            case PT::SystemRomLo: src = "System ROM, lower 16K"; break;
            case PT::SystemRomHi: src = "System ROM, upper 16K"; break;
            case PT::Bank3Rom: src = "BASIC ROM bank 3"; break;
            case PT::Bank3bRom: src = "BASIC ROM bank 3b (hidden)"; break;
            case PT::Bank6Rom: src = "ROM IV (bank 6)"; break;
            case PT::BusCard: src = "60-pin bus: " + (bs.busCard.empty() ? std::string("card") : bs.busCard); break;
            case PT::InternalRam: src = "Internal RAM"; kind = "RAM"; break;
            case PT::Slot1: src = slotSrc(1, bs.slotmapRedirect[p] ? true : (bank[p] & 1)); break;
            case PT::Slot2:
                // SLOT2MAP: mode 1 page C bank 1 and mode 2 page B bank 1 show
                // the low half, mode 2 page A bank 1 the high half.
                src = slotSrc(2, bs.slotmapRedirect[p] ? p == 0 : (bank[p] & 1));
                break;
        }
        if (bs.slotmapRedirect[p]) src += p == 1 && bs.target[p] == PT::Slot1 ? "  ◀ SLOT1MAP" : fmt("  ◀ SLOT2MAP mode %u", bs.slot2MapMode);
        t.addRow({std::string(1, pages[p]), range(p * 0x4000u, p * 0x4000u + 0x3FFF), fmt("%u", bank[p]), src, kind});
    }
    std::vector<std::string> out = t.render();
    addNote(out, fmt("Port 31H = %02X (bank select), 28H = %02X (Slot 2 vertical bank), 3CH = %02X (SLOTMAP)",
                      bs.port31, bs.port28, bs.port3c));
    addNote(out, std::string("Port 3DH b2 = ") + (bs.hiddenBasicRom ? "0: page B bank 3 is the hidden BASIC ROM 3b"
                                                                      : "1: page B bank 3 is BASIC ROM 3"));
    addNote(out, bs.slot1MapRemapped ? "SLOT1MAP on: Slot 1's upper 16K also answers at page B bank 1"
                                      : "SLOT1MAP off: Slot 1 only at page C banks 0/1");
    switch (bs.slot2MapMode) {
        case 1: addNote(out, "SLOT2MAP mode 1: Slot 2's lower 16K also answers at page C bank 1"); break;
        case 2: addNote(out, "SLOT2MAP mode 2: Slot 2 also at page B bank 1 (lower 16K) and page A bank 1 (upper 16K)"); break;
        default: addNote(out, "SLOT2MAP mode 0: Slot 2 only at page C banks 2/3"); break;
    }
    return out;
}

// ── LH5803 view ──────────────────────────────────────────────────────────

/// The cards answering an LH5803 window, in `step`-byte runs.
auto scanBus(const Ctx& c, uint32_t lo, uint32_t hi, bool me1, uint32_t step) {
    const LH5803SharedMemory& lh = c.m.lh5803Memory();
    return scan(lo, hi, step, [&](uint16_t a) { return lh.debugBusCardAt(a, me1, lh.pu(), lh.pv()); });
}

/// What Z-80 page C shows with `bank` selected (PC1600Memory::read()'s
/// page-C branches, SLOTMAP mode 1 included): the LH5803's 0000H-3FFFH.
std::string pageCSourceFor(const Ctx& c, unsigned bank) {
    auto slot = [&](int s, bool high) {
        const SlotInfo& i = c.slot[s];
        std::string x = i.label(s) + fmt(", bank %u, %s 16K", bank, high ? "upper" : "lower");
        if (s == 2 && i.portBanked()) x += fmt(", vertical bank %d", i.bank);
        return x;
    };
    if (c.bs.slot2MapMode == 1 && bank == 1 && c.slot[2].card) return slot(2, false) + " (SLOT2MAP mode 1)";
    if (bank <= 1 && c.slot[1].card) return slot(1, bank & 1);
    if ((bank == 2 || bank == 3) && c.slot[2].card) return slot(2, bank & 1);
    if (bank == 6) return "ROM IV (bank 6)";
    return fmt("open bus (page C bank %u)", bank);
}

/// Page C's bank while the LH5803 runs a PC-1500 statement. In MODE 1,
/// P_MAPPRG (LH5803 E63CH) sets port 31H to (the first non-zero ADTBL entry
/// AND 70H) OR 06H for the statement and restores it afterwards; in MODE 0
/// (or with no ADTBL entry) page C stays as the Z-80 left it. -1 = unchanged.
int statementPageCBank(const Ctx& c) {
    if (!(c.wa(0xF1BC) & 0x40)) return -1;
    for (uint16_t a = 0xF1D6; a <= 0xF1DA; ++a)
        if (const uint8_t e = c.wa(a)) return ((e & 0x70) | 0x06) >> 4 & 7;
    return -1;
}

/// Whether `MODE 1` would be accepted now: PC15MAP (P0-B0 1676H) refuses
/// (ERROR 110) unless S0MTB (F02AH) is 5, i.e. S0 spans at most one module
/// bank. Bit 6 of BMODE set without that is the POKE-forced MODE 1
/// (PC-1600-MODE0-MODE1.md §6).
std::string mode1Line(const Ctx& c) {
    const bool on = c.wa(0xF1BC) & 0x40;
    const uint8_t mtb = c.wa(0xF02A);
    if (on && mtb == 5) return "MODE 1 is on.";
    if (on)
        return fmt("MODE 1 is forced (BMODE b6 set, e.g. POKE &F1BC): S0MTB = %02XH, so the memory layout is "
                   "MODE 0's and the LH5803 sees only the first program bank.", mtb);
    if (mtb == 5) return "MODE 0. MODE 1 would be accepted (S0MTB = 05H: S0 spans at most one module bank).";
    return fmt("MODE 0. MODE 1 would be refused with ERROR 110: S0 spans more than one module bank "
               "(S0MTB = %02XH, not 05H; PC15MAP 1676H). The CE-150 / CE-158 still run here statement by "
               "statement, except CSAVE, CLOAD, MERGE, CHAIN, LLIST, TERMINAL and DTE.", mtb);
}

std::vector<std::string> lhView(const Ctx& c) {
    const LH5803SharedMemory& lh = c.m.lh5803Memory();
    TextTable t({"Space", "LH5803", "Answers", "Note"});
    t.addSpan("ME0");
    // What the LH5803 finds at 0000H-3FFFH depends on what it runs: only a
    // PC-1500 statement (X_EXCOMM_GO, DC8BH) and XPEEK (DCA0H) call
    // P_MAPPRG; functions, comparisons, arithmetic and the power-off path
    // see page C as the Z-80 left it.
    t.addRow({"ME0", "0000–3FFF", "", ""});
    {
        const int b = statementPageCBank(c);
        const bool mode1 = c.wa(0xF1BC) & 0x40;
        t.addRow({"", "  statements, XPEEK",
                  "Z80 8000–BFFF: " + pageCSourceFor(c, b < 0 ? c.bs.pageCBank : unsigned(b)),
                  b >= 0 ? "MODE 1: P_MAPPRG maps the program bank"
                  : mode1 ? "MODE 1, no ADTBL entry: page C as it is"
                          : "MODE 0: page C as it is"});
    }
    t.addRow({"", "  functions, arithmetic", "Z80 8000–BFFF: " + pageCSourceFor(c, c.bs.pageCBank),
              "always page C as it is"});
    t.addRow({"ME0", "4000–7FFF", "Internal RAM (Z80 C000–FFFF)", ""});
    t.addRow({"", "  7400–744F", "→ 7600–764F", "LHA90 alias"});
    t.addRow({"", "  7500–754F", "→ 7700–774F", "LHA90 alias"});
    t.addRow({"", "  7600–764F", "PC-1500 display RAM", "ME0 writes also drawn on the LCD"});
    for (const auto& r : scanBus(c, 0x8000, 0xBFFF, false, 0x400))
        t.addRow({"ME0", range(r.lo, r.hi), r.who ? cardName(*r.who) + " ROM" : "open bus",
                  fmt("peripheral window, PU=%d PV=%d", lh.pu(), lh.pv())});
    t.addRow({"ME0", "C000–FFFF", "LH5803 ROM", ""});

    t.addSpan("ME1 (floats except below: keeps the bus's last byte)");
    t.addRow({"ME1", "0020–0027, 0033", "UART TC8576F / sub-CPU answer", "also A020–A027, A033"});
    t.addRow({"ME1", "A030–A03F", "SC7852 ports 30H–3FH", "A038 = bus handoff to the SC7852"});
    t.addRow({"ME1", "8040–805F, A040–A05F", "LCD ports 40H–5FH", ""});
    for (const auto& r : scanBus(c, 0x8000, 0xFFFF, true, 8))
        if (r.who) t.addRow({"ME1", range(r.lo, r.hi), cardName(*r.who), "card I/O"});
    t.addRow({"ME1", "F000–F00F", "SC7852 ports 10H–1FH", "the LH5810-compatible block"});
    std::vector<std::string> out = t.render();
    addNote(out, fmt("PU = %d, PV = %d (the LH5803's flip-flops; the ROM sets PV from CALLH's PARBAN)", lh.pu(), lh.pv()));
    addNote(out, c.m.sc7852Owns() ? "Bus owner: the SC7852 (the LH5803 waits)" : "Bus owner: the LH5803");
    addNote(out, mode1Line(c));
    return out;
}

// ── BASIC area (S0) ──────────────────────────────────────────────────────

/// A labelled piece of a segment, Z-80 addresses inclusive.
struct Piece {
    uint16_t lo, hi;
    std::string what;
};

std::string segmentName(const Ctx& c, const pc1600::ProgramSegment& s) {
    if (s.kind == pc1600::ProgramSegment::Kind::InternalRam) return "Internal RAM";
    return c.slot[s.slot].label(s.slot) + fmt(", bank %d", s.adtblBank);
}

/// The plan's segments holding the program's first byte and its FFH mark:
/// the ADTBL index the ROM keeps with each address, and the address itself
/// (internal RAM continues index 5).
void programSegments(const S0Figures& f, const std::vector<pc1600::ProgramSegment>& segs, size_t* first,
                     size_t* last) {
    *first = *last = segs.size();
    for (size_t k = 0; k < segs.size(); ++k) {
        const pc1600::ProgramSegment& s = segs[k];
        const auto holds = [&](uint16_t a) { return a >= s.base && a <= s.top; };
        if (*first == segs.size() && s.adtblIndex == f.startIndex && holds(f.start)) *first = k;
        if (s.adtblIndex == f.endIndex && holds(f.end)) *last = k;
    }
}

/// The pieces of S0 segment `k` of `segs` from bottom to top.
std::vector<Piece> s0Pieces(const S0Figures& f, const std::vector<pc1600::ProgramSegment>& segs, size_t k) {
    const pc1600::ProgramSegment& s = segs[k];
    size_t first, last;
    programSegments(f, segs, &first, &last);
    std::vector<Piece> out;
    const bool internal = s.kind == pc1600::ProgramSegment::Kind::InternalRam;
    uint16_t lo = s.windowBase;
    const uint16_t top = internal ? uint16_t(kWorkArea - 1) : s.top;
    auto add = [&](uint16_t hi, std::string what) {
        if (hi >= lo && hi <= top) out.push_back({lo, hi, std::move(what)});
        lo = uint16_t(hi + 1);
    };
    if (k == 0) {
        if (f.mlStart > lo && f.mlStart <= s.base) add(uint16_t(f.mlStart - 1), "header + system reserve");
        if (s.base > lo) add(uint16_t(s.base - 1), "machine-language reserve");
    }
    if (first < segs.size() && last < segs.size() && k >= first && k <= last) {
        if (k == first && k == last) add(f.end, "program (BASPRG_ST … BASPRG_END = FFH)");
        else if (k == last) add(f.end, "program, up to BASPRG_END (FFH)");
        else add(s.top, "program, continued in the next bank");
    }
    if (internal) {
        if (f.varPtr > lo && f.varPtr <= kWorkArea) add(uint16_t(f.varPtr - 1), "free");
        if (f.ramEnd > lo && f.ramEnd <= kWorkArea) add(uint16_t(f.ramEnd - 1), "variables (VARPTR … RAM_END:00)");
        add(uint16_t(kWorkArea - 1), "above RAM_END");
    } else {
        add(top, "free");
    }
    return out;
}

std::vector<std::string> basicAreaView(const Ctx& c) {
    const pc1600::PlacementResult plan = pc1600::planS0Placement(c.placementInput(), {});
    if (!plan.ok) return {"BASIC area: " + plan.error};
    const S0Figures f = s0Figures(c);
    TextTable t({"Z80", "LH5803", "Size", "Holds"}, {R::Left, R::Left, R::Right, R::Left});
    for (size_t k = 0; k < plan.segments.size(); ++k) {
        const pc1600::ProgramSegment& s = plan.segments[k];
        t.addSpan(fmt("%s — ADTBL+%d", segmentName(c, s).c_str(), s.adtblIndex));
        for (const Piece& p : s0Pieces(f, plan.segments, k))
            t.addRow({range(p.lo, p.hi), range(pc1600::z80ToLh5803(p.lo), pc1600::z80ToLh5803(p.hi)),
                      sizeLabel(uint32_t(p.hi - p.lo + 1)), p.what});
    }
    t.addSpan("Work area");
    t.addRow({range(kWorkArea, 0xFFFF), range(0x7000, 0x7FFF), "4K", "BASIC / IOCS work area, stacks, buffers"});
    std::vector<std::string> out = t.render();
    const CoreDebug::PC1600ProgramAreas areas = CoreDebug::readPC1600ProgramAreas([&](uint16_t a) { return c.wa(a); });
    addNote(out, fmt("Program %d bytes, MEM %d bytes free. A program never straddles a module bank: each bank's "
                      "part ends with a 00 00 mark.",
                      f.programBytes, areas.memS0));
    return out;
}

// ── Program areas (TITLE) ────────────────────────────────────────────────

std::vector<std::string> programAreasView(const Ctx& c) {
    const CoreDebug::PC1600ProgramAreas areas = CoreDebug::readPC1600ProgramAreas([&](uint16_t a) { return c.wa(a); });
    const S0Figures f = s0Figures(c);
    TextTable t({"Area", "Where", "Start", "End", "Limit", "Program", "Free", ""},
                {R::Left, R::Left, R::Left, R::Left, R::Left, R::Right, R::Right, R::Left});
    const pc1600::PlacementResult plan = pc1600::planS0Placement(c.placementInput(), {});
    std::string where;
    for (const pc1600::ProgramSegment& s : plan.segments)
        where += (where.empty() ? "" : " → ") +
                 (s.kind == pc1600::ProgramSegment::Kind::InternalRam ? std::string("internal")
                                                                       : fmt("S%d b%d", s.slot, s.adtblBank));
    const char* mark = "◀ TITLE";
    t.addRow({"S0", plan.ok ? where : plan.error, fmt("%04X [%d]", f.start, f.startIndex),
              fmt("%04X [%d]", f.end, f.endIndex), fmt("%04X", f.ramEnd), fmt("%d", f.programBytes),
              fmt("%d", areas.memS0), areas.title == 0 ? mark : ""});
    for (int i = 0; i < 2; ++i) {
        const CoreDebug::PC1600SlotProgramArea& sl = areas.slot[i];
        const SlotInfo& si = c.slot[i + 1];
        if (!sl.programModule()) {
            t.addRow({fmt("S%d", i + 1), si.card ? si.label(i + 1) : fmt("Slot %d empty", i + 1),
                      sl.mtb == 0xFE ? "folded into S0" : "no program module", "", "", "", "", ""});
            continue;
        }
        const int bytes = int(sl.end) - int(sl.start) + (sl.endIndex - sl.startIndex) * int(kBank) + 1;
        t.addRow({fmt("S%d", i + 1), si.label(i + 1) + fmt(" (ADTBL+%d…+%d)", sl.mtb, sl.limitIndex),
                  fmt("%04X [%u]", sl.start, sl.startIndex), fmt("%04X [%u]", sl.end, sl.endIndex),
                  fmt("%02X00 [%u]", sl.limitPage, sl.limitIndex), fmt("%d", bytes), fmt("%d", sl.freeBytes),
                  areas.title == i + 1 ? mark : ""});
    }
    std::vector<std::string> out = t.render();
    addNote(out, "Addresses are Z-80; [n] is the ADTBL index. S1/S2 are program modules (INIT \"Sn:\",\"P\"); "
                  "TITLE picks the one RUN / LIST / LOAD work on, MEM counts S0 only.");
    return out;
}

// ── RAM disks ────────────────────────────────────────────────────────────

/// A RAM disk's place in its module and what its boot sector says. F055H /
/// F05BH count the module's RAM that is NOT disk (expansion or program
/// memory), in 2 KB units from the bottom: INIT "Sn:","F" leaves 0, "M" /
/// "P" add the module's size, capped at 10H = 32K, one vertical bank (P1-B3
/// 4B9A-4BBC; the Filesystem doc's "RAM-disk size" reading doesn't match
/// the ROM). The disk is the rest of the module, and counts only with its
/// boot sector.
struct Disk {
    int slot = 0;
    uint32_t ramPart = 0; // F055H / F05BH × 2 KB
    uint32_t size = 0;    // the module beyond ramPart
    uint32_t offset = 0;  // into the module image (= ramPart)
    bool present = false; // a module with RAM beyond ramPart
    bool boot = false;    // the boot sector's 55H 80H header is there
    uint8_t media = 0, maxDir = 0;
    uint16_t maxCls = 0, fatSector = 0, dirSector = 0, dataSector = 0;
};

Disk diskOf(const Ctx& c, int slot) {
    Disk d;
    d.slot = slot;
    d.ramPart = uint32_t(c.wa(slot == 1 ? 0xF055 : 0xF05B)) * 2048;
    const SlotInfo& si = c.slot[slot];
    if (si.image.size() <= d.ramPart + 0x200) return d;
    d.present = true;
    d.offset = d.ramPart;
    d.size = uint32_t(si.image.size()) - d.ramPart;
    const uint8_t* b = si.image.data() + d.offset;
    d.boot = b[0] == 0x55 && b[1] == 0x80;
    if (d.boot) {
        // PC-1600-Filesystem.md §5.1.
        d.media = b[0x08];
        d.fatSector = uint16_t(b[0x0F] | b[0x10] << 8);
        d.maxDir = b[0x12];
        d.dataSector = uint16_t(b[0x13] | b[0x14] << 8);
        d.maxCls = uint16_t(b[0x15] | b[0x16] << 8);
        d.dirSector = uint16_t(b[0x18] | b[0x19] << 8);
    }
    return d;
}

/// "vertical banks 1–7 (port 28H)" or "module 0000–7FFF".
std::string diskWhere(const Ctx& c, const Disk& d) {
    const SlotInfo& si = c.slot[d.slot];
    const uint32_t end = d.offset + d.size - 1;
    if (si.portBanked() && si.geom.bankSize) {
        const uint32_t lo = d.offset / si.geom.bankSize, hi = end / si.geom.bankSize;
        return lo == hi ? fmt("vertical bank %u", lo) : fmt("vertical banks %u–%u", lo, hi);
    }
    return "module " + range(d.offset, end, 5);
}

std::vector<std::string> diskAreasView(const Ctx& c) {
    TextTable t({"Drive", "Module", "Size", "Where", "Boot sector"});
    for (int s = 1; s <= 2; ++s) {
        const Disk d = diskOf(c, s);
        const SlotInfo& si = c.slot[s];
        const std::string drive = fmt("S%d:", s);
        if (!si.card) {
            t.addRow({drive, fmt("Slot %d empty", s), "—", "no RAM disk"});
            continue;
        }
        if (!d.present || !d.boot) {
            t.addRow({drive, si.label(s), "—",
                      fmt("no RAM disk (%s of the module is RAM%s)", sizeLabel(d.ramPart).c_str(),
                          d.present ? ", the rest has no boot sector" : "")});
            continue;
        }
        t.addRow({drive, si.label(s), sizeLabel(d.size), diskWhere(c, d),
                  fmt("media %02XH, %u clusters, %u dir entries; FAT sector %u, dir %u, data %u", d.media, d.maxCls,
                      d.maxDir, d.fatSector, d.dirSector, d.dataSector)});
    }
    std::vector<std::string> out = t.render();
    const SlotInfo& s2 = c.slot[2];
    if (s2.portBanked())
        addNote(out, fmt("Slot 2 switches its vertical banks with OUT (28H); latched now: %d (last OUT 28H = %u). "
                          "Only the file system switches them; vertical bank 0 is the one BASIC sees.",
                          s2.bank, c.bs.port28));
    addNote(out, "A RAM disk is the module above its RAM part (F055H / F05BH): INIT \"Sn:\",\"F\" makes the whole "
                  "module a disk, \"M\" / \"P\" give the lower 32K back to BASIC.");
    return out;
}

// ── Physical RAM ─────────────────────────────────────────────────────────

/// A claim on a stretch of one RAM: chip offsets, inclusive.
struct Claim {
    uint32_t lo, hi;
    std::string what;
};

/// The module-image offset of Z-80 `addr` in global page-C `bank`, at
/// vertical bank 0, for a module whose image is laid out bank by bank (the
/// slot cards: 16K halves by PVOUT, vertical banks above). A window
/// smaller than 16K sits at the top of 8000H-BFFFH.
uint32_t moduleOffset(const SlotInfo& si, int bank, uint16_t addr) {
    const uint32_t win = si.geom.bankSize ? si.geom.bankSize : si.geom.imageSize;
    if (win < kBank) return uint32_t(addr) - (0xC000u - win);
    return uint32_t(bank & 1) * kBank + (addr - 0x8000u);
}

std::vector<std::string> physicalRamView(const Ctx& c) {
    std::vector<Claim> chips[3];  // [0] internal RAM, [1]/[2] slot modules
    const S0Figures f = s0Figures(c);
    const pc1600::PlacementResult plan = pc1600::planS0Placement(c.placementInput(), {});
    if (plan.ok) {
        for (size_t k = 0; k < plan.segments.size(); ++k) {
            const pc1600::ProgramSegment& s = plan.segments[k];
            for (const Piece& p : s0Pieces(f, plan.segments, k)) {
                if (s.kind == pc1600::ProgramSegment::Kind::InternalRam)
                    chips[0].push_back({p.lo - 0xC000u, p.hi - 0xC000u, "S0: " + p.what});
                else
                    chips[s.slot].push_back({moduleOffset(c.slot[s.slot], s.adtblBank, p.lo),
                                             moduleOffset(c.slot[s.slot], s.adtblBank, p.hi), "S0: " + p.what});
            }
        }
    }
    chips[0].push_back({kWorkArea - 0xC000u, 0x3FFF, "work area F000–FFFF"});
    for (int s = 1; s <= 2; ++s) {
        const SlotInfo& si = c.slot[s];
        if (!si.card) continue;
        const pc1600::PlacementResult area = pc1600::planModuleRegionPlacement(c.placementInput(), s, {});
        if (area.ok) {
            // The module's own program area, laid out like S0's: header and
            // reserve, ML reserve, program, free up to the limit.
            const pc1600::SlotDescriptor d = pc1600::readSlotDescriptor([&](uint16_t a) { return c.wa(a); }, s);
            S0Figures mf;
            mf.start = d.start;
            mf.end = d.end;
            mf.startIndex = d.startIndex;
            mf.endIndex = d.endIndex;
            mf.mlStart = uint16_t(area.segments.front().windowBase + 0xC5);
            for (size_t k = 0; k < area.segments.size(); ++k) {
                const pc1600::ProgramSegment& seg = area.segments[k];
                for (const Piece& p : s0Pieces(mf, area.segments, k))
                    chips[s].push_back({moduleOffset(si, seg.adtblBank, p.lo), moduleOffset(si, seg.adtblBank, p.hi),
                                        fmt("S%d: %s", s, p.what.c_str())});
            }
        }
        const Disk d = diskOf(c, s);
        if (d.present && d.boot) chips[s].push_back({d.offset, d.offset + d.size - 1, fmt("RAM disk S%d:", s)});
    }

    TextTable t({"Offset", "Size", "Holds"}, {R::Left, R::Right, R::Left});
    auto chip = [&](const std::string& title, std::vector<Claim> claims, uint32_t size, int digits) {
        t.addSpan(title);
        std::sort(claims.begin(), claims.end(), [](const Claim& a, const Claim& b) { return a.lo < b.lo; });
        uint32_t next = 0;
        for (const Claim& cl : claims) {
            if (cl.hi >= size || cl.lo < next) continue;  // overlapping / outside: shown once
            if (cl.lo > next) t.addRow({range(next, cl.lo - 1, digits), sizeLabel(cl.lo - next), "unused"});
            t.addRow({range(cl.lo, cl.hi, digits), sizeLabel(cl.hi - cl.lo + 1), cl.what});
            next = cl.hi + 1;
        }
        if (next < size) t.addRow({range(next, size - 1, digits), sizeLabel(size - next), "unused"});
    };
    chip("Internal RAM (16K, Z80 C000–FFFF)", chips[0], 0x4000, 4);
    for (int s = 1; s <= 2; ++s) {
        const SlotInfo& si = c.slot[s];
        if (!si.card || si.image.empty()) continue;
        std::string title = si.label(s) + " (" + sizeLabel(uint32_t(si.image.size()));
        if (si.portBanked()) title += fmt(", %d vertical banks of %s", si.bankCount, sizeLabel(si.geom.bankSize).c_str());
        chip(title + ")", chips[s], uint32_t(si.image.size()), si.image.size() > 0x10000 ? 5 : 4);
    }
    std::vector<std::string> out = t.render();
    addNote(out, "Offsets into each RAM; a slot module's are into its image (16K halves by PVOUT, vertical banks above). "
                  "\"unused\" is RAM no area claims.");
    return out;
}

// ── Dumps ────────────────────────────────────────────────────────────────

std::vector<std::string> dumpSegments(const Ctx& c, const std::vector<pc1600::ProgramSegment>& segs, const char* what) {
    std::vector<std::string> out = {fmt("── %s ──", what)};
    for (const pc1600::ProgramSegment& s : segs) {
        const bool internal = s.kind == pc1600::ProgramSegment::Kind::InternalRam;
        const uint16_t lo = s.windowBase, hi = internal ? uint16_t(kWorkArea - 1) : s.top;
        out.push_back(fmt("── %s, Z80 %s ──", segmentName(c, s).c_str(), range(lo, hi).c_str()));
        std::vector<uint8_t> bytes;
        bytes.reserve(hi - lo + 1u);
        for (uint32_t a = lo; a <= hi; ++a) bytes.push_back(c.read(s.adtblBank, uint16_t(a)));
        const auto rows = hexDump(bytes, lo);
        out.insert(out.end(), rows.begin(), rows.end());
    }
    return out;
}

std::vector<std::string> dumpProgram(const Ctx& c, int slot) {
    const pc1600::PlacementResult plan = pc1600::planModuleRegionPlacement(c.placementInput(), slot, {});
    if (!plan.ok) return {fmt("S%d: %s", slot, plan.error.c_str())};
    return dumpSegments(c, plan.segments, fmt("Program area S%d", slot).c_str());
}

std::vector<std::string> dumpDisk(const Ctx& c, int slot) {
    const Disk d = diskOf(c, slot);
    if (!d.present || !d.boot) return {fmt("S%d: no RAM disk", slot)};
    const SlotInfo& si = c.slot[slot];
    std::vector<std::string> out = {fmt("── RAM disk S%d:, %s, %s, module offsets ──", slot, sizeLabel(d.size).c_str(),
                                        diskWhere(c, d).c_str())};
    const uint32_t chunk = si.portBanked() && si.geom.bankSize ? si.geom.bankSize : d.size;
    for (uint32_t off = d.offset; off < d.offset + d.size; off = (off / chunk + 1) * chunk) {
        const uint32_t end = std::min(d.offset + d.size, (off / chunk + 1) * chunk);
        if (chunk != d.size) out.push_back(fmt("── vertical bank %u ──", off / chunk));
        const auto rows = hexDump(si.image.data() + off, end - off, off, 5);
        out.insert(out.end(), rows.begin(), rows.end());
    }
    return out;
}

}  // namespace

std::vector<std::string> pc1600View(const PC1600Machine& m, View v) {
    const Ctx c(m);
    auto body = [&](std::vector<std::string> lines) { return titled(v, false, std::move(lines)); };
    switch (v) {
        case View::Pointers: return body(pointersView(c));
        case View::Inventory: return body(inventoryView(c));
        case View::Z80View: return body(z80View(c));
        case View::LhView: return body(lhView(c));
        case View::BasicArea: return body(basicAreaView(c));
        case View::ProgramAreas: return body(programAreasView(c));
        case View::DiskAreas: return body(diskAreasView(c));
        case View::PhysicalRam: return body(physicalRamView(c));
        case View::DumpBasicArea: {
            const pc1600::PlacementResult plan = pc1600::planS0Placement(c.placementInput(), {});
            if (!plan.ok) return {"BASIC area: " + plan.error};
            return dumpSegments(c, plan.segments, "BASIC area S0");
        }
        case View::DumpProgramS1: return dumpProgram(c, 1);
        case View::DumpProgramS2: return dumpProgram(c, 2);
        case View::DumpDiskS1: return dumpDisk(c, 1);
        case View::DumpDiskS2: return dumpDisk(c, 2);
        case View::DumpMlArea: break;
    }
    return {fmt("%s: not on the PC-1600", viewTitle(v))};
}

std::vector<MenuEntry> pc1600Menu(const PC1600Machine& m, bool dumps) {
    if (!dumps)
        return {{View::Inventory, true},   {View::Z80View, true},      {View::LhView, true},
                {View::BasicArea, true},   {View::ProgramAreas, true}, {View::DiskAreas, true},
                {View::PhysicalRam, true}};
    const Ctx c(m);
    auto programModule = [&](int slot) {
        return pc1600::readSlotDescriptor([&](uint16_t a) { return c.wa(a); }, slot).programModule();
    };
    return {{View::DumpBasicArea, true},
            {View::DumpProgramS1, programModule(1)},
            {View::DumpProgramS2, programModule(2)},
            {View::DumpDiskS1, diskOf(c, 1).boot},
            {View::DumpDiskS2, diskOf(c, 2).boot}};
}

}  // namespace inspect
