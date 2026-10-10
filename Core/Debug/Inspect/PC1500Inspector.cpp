#include "PC1500Inspector.hpp"

#include <algorithm>

#include "../../Connector/PC1500SignalDecode.hpp"
#include "../../PC1500/PC1500Machine.hpp"
#include "HexDump.hpp"
#include "TextTable.hpp"

namespace inspect {
namespace {

using R = TextTable::Align;

constexpr uint16_t kUserRam = 0x4000;
constexpr uint16_t kSystemRam = 0x7800;
constexpr uint16_t kMlArea = 0x7C00;

std::string range(uint32_t lo, uint32_t hi) { return fmt("%04X–%04X", unsigned(lo), unsigned(hi)); }

const char* kindName(CardMemory::Kind k) {
    switch (k) {
        case CardMemory::Kind::Rom: return "ROM";
        case CardMemory::Kind::Ram: return "RAM";
        case CardMemory::Kind::Flash: return "flash";
        case CardMemory::Kind::Mixed: return "mixed";
    }
    return "?";
}

struct Ctx {
    const PC1500Machine& m;
    bool a;              // PC-1500A
    uint32_t userRamEnd; // one past the built-in user RAM
    const ExpansionCard* card;
    std::string cardName;

    explicit Ctx(const PC1500Machine& machine)
        : m(machine),
          a(machine.variant() == PC1500Variant::PC1500A),
          userRamEnd(a ? 0x5800u : 0x4800u),
          card(machine.memory().expansionConnector().attachedCard()),
          cardName(card ? card->moduleName() : std::string()) {}

    uint8_t peek(uint16_t a) const { return m.memory().peek(a); }
    uint16_t be(uint16_t a) const { return uint16_t(peek(a) << 8 | peek(uint16_t(a + 1))); }

    std::string moduleLabel() const { return card ? "Module " + (cardName.empty() ? std::string("card") : cardName) : "Module slot"; }

    /// Who holds RAM address `addr` (0000H-7FFFH).
    std::string sourceOf(uint32_t addr) const {
        if (addr >= kUserRam && addr < userRamEnd) return "built-in";
        if (addr < 0x7000) return card ? (cardName.empty() ? "module" : cardName) : "open bus";
        return addr < kSystemRam ? "display RAM" : "system RAM";
    }
};

/// Runs of `[lo, hi]` in `step`-byte blocks that `answer(addr)` names the
/// same: {lo, hi, name}.
struct Run {
    uint32_t lo, hi;
    std::string name;
};

template <class F>
std::vector<Run> scan(uint32_t lo, uint32_t hi, uint32_t step, F answer) {
    std::vector<Run> runs;
    for (uint32_t a = lo; a <= hi; a += step) {
        const std::string name = answer(uint16_t(a));
        if (!runs.empty() && runs.back().name == name && runs.back().hi + 1 == a)
            runs.back().hi = a + step - 1;
        else
            runs.push_back({a, a + step - 1, name});
    }
    return runs;
}

/// The 60-pin card that answers an LH5801 cycle, "" for none (a register a
/// read would disturb counts as answering, without being read).
std::string busCardAt(const Ctx& c, uint16_t addr, bool me1, bool pu, bool pv) {
    const SystemBusPins pins = PC1500SignalDecode::systemBusPins(addr, false, me1, pu, pv);
    for (const SystemBusCard* card : c.m.memory().systemBus().chain()) {
        uint8_t v;
        if (card->readHasSideEffects(pins) || card->respondsToRead(pins, v))
            return card->moduleName().empty() ? "card" : card->moduleName();
    }
    return {};
}

// ── Pointers ─────────────────────────────────────────────────────────────

enum class V : uint8_t { Byte, Word };
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
    bool (*shown)(const PC1500Machine&) = nullptr;  // null: always
};

std::string decodeOnOff(uint8_t v) { return v ? "on" : "off"; }
std::string decodeLock(uint8_t v) { return v == 0xFF ? "unlocked" : "locked: the MODE key is blocked"; }
std::string decodeMode(uint8_t v) {
    std::string s;
    for (const auto& [bit, name] : {std::pair<int, const char*>{6, "RUN"}, {5, "PRO"}, {4, "RESERVE"}})
        if (v & (1u << bit)) s += (s.empty() ? "" : " ") + std::string(name);
    return s.empty() ? "—" : s;
}
std::string decodeOpn(uint8_t v) {
    switch (v) {
        case 0x60: return "LCD";
        case 0x5C: return "CMT";
        case 0x58: return "MGP";
        case 0xC4: return "LPRT";
        case 0xC0: return "COM";
        default: return "?";
    }
}

bool ce150On(const PC1500Machine& m) { return m.ce150Attached(); }
bool ce158On(const PC1500Machine& m) { return m.ce158Attached(); }

// Addresses and names from PC-1500.lib / CE-150.lib / CE-158.lib.
const std::vector<Group>& pc1500Pointers() {
    static const std::vector<Group> groups = {
        {"BASIC program",
         {{"BASPRG_ST", 0x7865, V::Word, "BASIC program start"},
          {"BASPRG_END", 0x7867, V::Word, "BASIC program end, the FFH mark"},
          {"BASPRG_EDT", 0x7869, V::Word, "Editor line-modification pointer"},
          {"CURR_TOP", 0x789E, V::Word, "Start of the program holding the current line"},
          {"CURR_LINE", 0x789C, V::Word, "Current line number"},
          {"PREV_LINE", 0x78A2, V::Word, "Previous line number"},
          {"SRCH_LINE", 0x78A8, V::Word, "Line found by a search"},
          {"DATA_PTR", 0x78BE, V::Word, "DATA statement cursor"},
          {"ROM_ST", 0x7861, V::Word, "BASIC program in a ROM module"},
          {"TRACE_ON", 0x788D, V::Byte, "TRON", decodeOnOff},
          {"TRACE_PARAM", 0x788E, V::Word, "Trace output vector"}}},
        {"Errors and BREAK",
         {{"ERL", 0x789B, V::Byte, "Error code"},
          {"BREAKPARAM", 0x788A, V::Byte, "BREAK flag"},
          {"BRK_ADD", 0x78AC, V::Word, "BREAK address"},
          {"BRK_LINE", 0x78AE, V::Word, "BREAK line"},
          {"ERR_ADD", 0x78B2, V::Word, "Error address"},
          {"ERR_LINE", 0x78B4, V::Word, "Error line"},
          {"ON_ERR_ADD", 0x78B8, V::Word, "ON ERROR GOTO target"},
          {"ON_ERR_LINE", 0x78BA, V::Word, "ON ERROR line"}}},
        {"Variables and stacks",
         {{"RAM_ST", 0x7863, V::Byte, "Page of the user RAM start"},
          {"RAM_END", 0x7864, V::Byte, "Page one past the user RAM top"},
          {"VAR_START", 0x7899, V::Word, "Start of the variables (they grow down)"},
          {"CURVARADD", 0x7883, V::Word, "Currently accessed variable"},
          {"CURVARTYPE", 0x7885, V::Byte, "Its type"},
          {"STK_PTR_GSB_FOR", 0x7882, V::Byte, "Stack pointer for GOSUB and FOR"},
          {"FORNXT_STK_PTR", 0x7890, V::Byte, "FOR/NEXT stack pointer (low byte)"},
          {"GOSB_STK_PTR_L", 0x7891, V::Byte, "GOSUB stack pointer (low byte)"},
          {"STR_BUF_PTR_L", 0x7894, V::Byte, "String buffer pointer (low byte, 7B10H-7B5FH)"},
          {"INBUFPTR_L", 0x788B, V::Byte, "Input buffer pointer (low byte, 7BB0H-7BFFH)"}}},
        {"Mode and system",
         {{"Annunciators", 0x764F, V::Byte, "Mode", decodeMode},
          {"LOCK", 0x79FF, V::Byte, "", decodeLock},
          {"DISPARAM", 0x7880, V::Byte, "Display at READY"},
          {"PU_PV", 0x79D0, V::Byte, "PU/PV flag, ROM bank (00 ROM 1, 01 ROM 2)"},
          {"OPN", 0x79D1, V::Byte, "OPN device", decodeOpn},
          {"WAIT_CFG", 0x7871, V::Byte, "WAIT setting"},
          {"CURSOR_PTR", 0x7875, V::Byte, "Cursor column"}}},
        {"CE-150",
         {{"USER_CTRX", 0x79E0, V::Word, "Pen X"},
          {"USER_CTRY", 0x79E2, V::Word, "Pen Y"},
          {"CURR_PEN", 0x79EC, V::Byte, "Pen: 00 up, 01 down"},
          {"PRNT_MODE", 0x79F0, V::Byte, "00 TEXT, FF GRAPH"},
          {"PRNT_ROTATE", 0x79F2, V::Byte, "ROTATE"},
          {"PRNT_COLOR", 0x79F3, V::Byte, "COLOR"},
          {"PRNT_CSIZE", 0x79F4, V::Byte, "CSIZE"}},
         ce150On},
        {"CE-158",
         {{"OUTSTAT_REG", 0x7850, V::Byte, "OUTSTAT"},
          {"RS232C", 0x7851, V::Byte, "Console 1"},
          {"CONSOLE2", 0x7852, V::Byte, "Console 2"},
          {"ZONE_REG", 0x7856, V::Byte, "ZONE"},
          {"SETDEV_REG", 0x7857, V::Byte, "SETDEV"},
          {"SETCOM_REG", 0x7858, V::Byte, "SETCOM"}},
         ce158On},
    };
    return groups;
}

struct Figures {
    uint16_t ramSt, ramEnd, start, end, varStart, mlStart;
};

Figures figures(const Ctx& c) {
    Figures f;
    f.ramSt = uint16_t(c.peek(0x7863) << 8);
    f.ramEnd = uint16_t(c.peek(0x7864) << 8);
    f.start = c.be(0x7865);
    f.end = c.be(0x7867);
    f.varStart = c.be(0x7899);
    f.mlStart = uint16_t(f.ramSt + 0xC5);  // NEW 0: RAM start + C5H (Address-Decoding §5.4)
    return f;
}

std::vector<std::string> pointersView(const Ctx& c) {
    TextTable t({"Name", "Addr", "Value", "Meaning"});
    for (const Group& g : pc1500Pointers()) {
        if (g.shown && !g.shown(c.m)) continue;
        t.addSpan(g.title);
        for (const Ptr& p : g.ptrs) {
            std::string note = p.note;
            if (p.decode) {
                const std::string d = p.decode(c.peek(p.addr));
                note = note.empty() ? d : note + ": " + d;
            }
            const std::string value = p.value == V::Byte ? fmt("%02X", c.peek(p.addr)) : fmt("%04X", c.be(p.addr));
            t.addRow({p.name, fmt("%04X", p.addr), value, note});
        }
    }
    const Figures f = figures(c);
    t.addSpan("Computed");
    t.addRow({"MEM", "", fmt("%d", int(f.ramEnd) - int(f.end) - 1), "RAM_END:00 − BASPRG_END − 1"});
    t.addRow({"Program", "", fmt("%d", int(f.end) - int(f.start) + 1), "BASIC program, with the FFH mark"});
    const int ml = int(f.start) - int(f.mlStart);
    t.addRow({"ML reserve", "", fmt("%d", ml > 0 ? ml : 0), fmt("Machine-language area %04X up to BASPRG_ST", f.mlStart)});
    const int vars = int(f.ramEnd) - int(f.varStart);
    t.addRow({"Variables", "", fmt("%d", vars > 0 ? vars : 0), "RAM_END:00 − VAR_START"});
    return t.render();
}

// ── Inventory ────────────────────────────────────────────────────────────

std::string runsText(const std::vector<Run>& runs, const std::string& name) {
    std::string s;
    for (const Run& r : runs)
        if (r.name == name) s += (s.empty() ? "" : ", ") + range(r.lo, r.hi);
    return s;
}

std::vector<std::string> inventoryView(const Ctx& c) {
    TextTable t({"Memory", "Kind", "Size", "Banks", "Seen at"}, {R::Left, R::Left, R::Right, R::Left, R::Left});
    t.addSpan("Built in");
    t.addRow({"System ROM", "ROM", "16K", "—", "C000–FFFF"});
    t.addRow({"User RAM", "RAM", c.a ? "6K" : "2K", "—", range(kUserRam, c.userRamEnd - 1)});
    t.addRow({"Display RAM", "RAM", "512 B", "—", "7000–77FF (four times; 7600–77FF used)"});
    t.addRow({"System RAM", "RAM", c.a ? "2K" : "1K", "—", c.a ? "7800–7FFF" : "7800–7BFF (again at 7C00–7FFF)"});

    t.addSpan("Memory slot (40-pin)");
    if (!c.card) {
        t.addRow({"Module slot", "empty"});
    } else {
        const std::vector<Run> runs = scan(0x0000, 0xBFFF, 0x400, [&](uint16_t a) {
            return c.m.memory().debugSlotResponds(a) ? std::string("x") : std::string();
        });
        const std::string seen = runsText(runs, "x");
        const std::vector<CardMemory> mems = c.card->debugMemories();
        if (mems.empty()) t.addRow({c.moduleLabel(), "I/O only", "", "", seen});
        for (size_t k = 0; k < mems.size(); ++k) {
            const CardMemory& cm = mems[k];
            t.addRow({k == 0 ? c.moduleLabel() : "  " + cm.name, kindName(cm.kind), sizeLabel(cm.size),
                      cm.banks > 1 ? fmt("%u × %s", cm.banks, sizeLabel(cm.size / cm.banks).c_str()) : "—",
                      k == 0 ? seen : ""});
        }
    }

    t.addSpan("60-pin bus");
    const auto& cards = c.m.memory().systemBus().chain();
    if (cards.empty()) t.addRow({"(nothing attached)"});
    for (const SystemBusCard* card : cards) {
        const std::string name = card->moduleName().empty() ? "card" : card->moduleName();
        std::string seen;
        for (uint16_t base : {uint16_t(0x8000), uint16_t(0xA000)}) {
            bool pvHit[2] = {false, false};
            for (int pv = 0; pv < 2; ++pv)
                for (int pu = 0; pu < 2; ++pu) {
                    uint8_t v;
                    if (card->respondsToRead(PC1500SignalDecode::systemBusPins(base, false, false, pu, pv), v))
                        pvHit[pv] = true;
                }
            if (!pvHit[0] && !pvHit[1]) continue;
            seen += (seen.empty() ? "" : " · ") + range(base, base + 0x1FFFu) +
                    (pvHit[0] != pvHit[1] ? fmt(" (PV=%d)", pvHit[1] ? 1 : 0) : "");
        }
        const std::vector<CardMemory> mems = card->debugMemories();
        if (mems.empty()) t.addRow({name, "I/O only"});
        for (size_t k = 0; k < mems.size(); ++k)
            t.addRow({k == 0 ? name : "  " + mems[k].name, kindName(mems[k].kind), sizeLabel(mems[k].size),
                      mems[k].banks > 1 ? fmt("%u (PU)", mems[k].banks) : "—", k == 0 ? (seen.empty() ? "I/O only" : seen) : ""});
    }
    return t.render();
}

// ── LH5801 view ──────────────────────────────────────────────────────────

std::vector<std::string> lhView(const Ctx& c) {
    const PC1500Memory& mem = c.m.memory();
    TextTable t({"Space", "LH5801", "Answers", "Note"});
    t.addSpan("ME0");
    auto moduleRuns = [&](uint32_t lo, uint32_t hi) {
        for (const Run& r : scan(lo, hi, 0x400, [&](uint16_t a) {
                 return mem.debugSlotResponds(a) ? c.moduleLabel() : std::string("open bus");
             }))
            t.addRow({"ME0", range(r.lo, r.hi), r.name, "module area"});
    };
    moduleRuns(0x0000, 0x3FFF);
    t.addRow({"ME0", range(kUserRam, c.userRamEnd - 1), "User RAM (built in)", ""});
    moduleRuns(c.userRamEnd, 0x6FFF);
    t.addRow({"ME0", "7000–77FF", "Display RAM", "512 B, four times; LCD at 7600–764F / 7700–774F"});
    t.addRow({"ME0", "7800–7FFF", "System RAM", c.a ? "" : "1K: 7C00–7FFF is 7800–7BFF again"});
    for (const Run& r : scan(0x8000, 0xBFFF, 0x400, [&](uint16_t a) { return busCardAt(c, a, false, mem.pu(), mem.pv()); }))
        t.addRow({"ME0", range(r.lo, r.hi), r.name.empty() ? "open bus" : r.name + " ROM",
                  fmt("60-pin bus, PU=%d PV=%d", mem.pu(), mem.pv())});
    t.addRow({"ME0", "C000–FFFF", "System ROM", ""});

    t.addSpan("ME1 (aliases ME0 except below)");
    for (const Run& r : scan(0x8000, 0xFFFF, 8, [&](uint16_t a) { return busCardAt(c, a, true, mem.pu(), mem.pv()); }))
        if (!r.name.empty()) t.addRow({"ME1", range(r.lo, r.hi), r.name, "card I/O"});
    t.addRow({"ME1", "F000–F00F", "LH5811 I/O ports", "any ME1 address with A12 = A13 = 1"});
    std::vector<std::string> out = t.render();
    out.push_back(fmt("PU = %d, PV = %d (the LH5801's flip-flops)", mem.pu(), mem.pv()));
    return out;
}

// ── BASIC area ───────────────────────────────────────────────────────────

struct Piece {
    uint32_t lo, hi;
    std::string what;
};

std::vector<Piece> basicPieces(const Ctx& c) {
    const Figures f = figures(c);
    std::vector<Piece> out;
    uint32_t lo = f.ramSt;
    auto add = [&](uint32_t hi, const char* what) {
        if (hi + 1 > lo && hi < f.ramEnd) out.push_back({lo, hi, what});
        if (hi + 1 > lo) lo = hi + 1;
    };
    if (f.mlStart <= f.start) add(f.mlStart - 1u, "system reserve (197 B)");
    add(f.start - 1u, "machine-language reserve");
    add(f.end, "program (BASPRG_ST … BASPRG_END = FFH)");
    add(f.varStart - 1u, "free");
    add(f.ramEnd - 1u, "variables (VAR_START … RAM_END:00)");
    return out;
}

std::vector<std::string> basicAreaView(const Ctx& c) {
    const Figures f = figures(c);
    TextTable t({"Address", "Size", "RAM", "Holds"}, {R::Left, R::Right, R::Left, R::Left});
    for (const Piece& p : basicPieces(c)) {
        // Split a piece where the RAM behind it changes (module / built-in).
        uint32_t lo = p.lo;
        for (uint32_t a = p.lo; a <= p.hi; ++a)
            if (a == p.hi || c.sourceOf(a + 1) != c.sourceOf(lo)) {
                t.addRow({range(lo, a), sizeLabel(a - lo + 1), c.sourceOf(lo), p.what});
                lo = a + 1;
            }
    }
    std::vector<std::string> out = t.render();
    out.push_back(fmt("RAM_ST:00 = %04X, RAM_END:00 = %04X; MEM %d bytes free.", f.ramSt, f.ramEnd,
                      int(f.ramEnd) - int(f.end) - 1));
    return out;
}

// ── Physical RAM ─────────────────────────────────────────────────────────

std::vector<std::string> physicalRamView(const Ctx& c) {
    TextTable t({"Address", "Size", "Holds"}, {R::Left, R::Right, R::Left});
    auto chip = [&](const std::string& title, uint32_t lo, uint32_t hi) {
        t.addSpan(title);
        uint32_t next = lo;
        for (const Piece& p : basicPieces(c)) {
            const uint32_t a = std::max(p.lo, lo), b = std::min(p.hi, hi);
            if (a > b) continue;
            if (a > next) t.addRow({range(next, a - 1), sizeLabel(a - next), "outside the BASIC area"});
            t.addRow({range(a, b), sizeLabel(b - a + 1), p.what});
            next = b + 1;
        }
        if (next <= hi) t.addRow({range(next, hi), sizeLabel(hi - next + 1), "outside the BASIC area"});
    };
    // The RAM below the display RAM in address order: the module's windows
    // around the built-in user RAM.
    for (const Run& r : scan(0x0000, 0x6FFF, 0x400, [&](uint16_t a) {
             if (a >= kUserRam && a < c.userRamEnd) return std::string("u");
             return c.card && c.m.memory().debugSlotResponds(a) ? std::string("m") : std::string();
         })) {
        if (r.name == "m") chip(c.moduleLabel() + ", " + range(r.lo, r.hi), r.lo, r.hi);
        if (r.name == "u") chip(fmt("User RAM (%s built in)", c.a ? "6K" : "2K"), r.lo, r.hi);
    }
    t.addSpan("Display RAM (512 B)");
    t.addRow({"7600–764F", "80 B", "LCD columns, first half"});
    t.addRow({"7700–774F", "80 B", "LCD columns, second half"});
    t.addRow({"7650–77AF", "", "fixed string variables (around the LCD bytes)"});
    t.addSpan(fmt("System RAM (%s)", c.a ? "2K" : "1K"));
    t.addRow({"7800–784F", "80 B", "CPU stack"});
    t.addRow({"7850–79FF", "432 B", "BASIC / peripheral work area (pointers above)"});
    t.addRow({"7A00–7A37", "56 B", "arithmetic registers"});
    t.addRow({"7A38–7AFF", "200 B", "BASIC stack"});
    t.addRow({"7B00–7B0F", "16 B", "keyboard / cursor work"});
    t.addRow({"7B10–7B5F", "80 B", "string buffer"});
    t.addRow({"7B60–7BAF", "80 B", "output buffer"});
    t.addRow({"7BB0–7BFF", "80 B", "input buffer"});
    t.addRow({"7C00–7FFF", "1K", c.a ? "machine-language area" : "7800–7BFF again (only 1K fitted)"});
    return t.render();
}

// ── Dumps ────────────────────────────────────────────────────────────────

std::vector<std::string> dumpRange(const Ctx& c, uint32_t lo, uint32_t hi) {
    std::vector<uint8_t> bytes;
    for (uint32_t a = lo; a <= hi; ++a) bytes.push_back(c.peek(uint16_t(a)));
    return hexDump(bytes, lo);
}

}  // namespace

std::vector<std::string> pc1500View(const PC1500Machine& m, View v) {
    const Ctx c(m);
    std::vector<std::string> out = {fmt("── %s ──", viewTitle(v, true))};
    auto body = [&](std::vector<std::string> lines) {
        out.insert(out.end(), lines.begin(), lines.end());
        return out;
    };
    switch (v) {
        case View::Pointers: return body(pointersView(c));
        case View::Inventory: return body(inventoryView(c));
        case View::LhView: return body(lhView(c));
        case View::BasicArea: return body(basicAreaView(c));
        case View::PhysicalRam: return body(physicalRamView(c));
        case View::DumpBasicArea: {
            const Figures f = figures(c);
            if (f.ramEnd <= f.ramSt) return body({"RAM_ST / RAM_END not set"});
            out[0] = fmt("── BASIC area %s ──", range(f.ramSt, f.ramEnd - 1u).c_str());
            return body(dumpRange(c, f.ramSt, f.ramEnd - 1u));
        }
        case View::DumpMlArea:
            out[0] = c.a ? "── Machine-language area 7C00–7FFF ──" : "── 7C00–7FFF (the 1K system RAM again) ──";
            return body(dumpRange(c, kMlArea, 0x7FFF));
        default: break;
    }
    return {fmt("%s: not on the PC-1500", viewTitle(v, true))};
}

std::vector<MenuEntry> pc1500Menu(const PC1500Machine&, bool dumps) {
    if (!dumps)
        return {{View::Inventory, true}, {View::LhView, true}, {View::BasicArea, true}, {View::PhysicalRam, true}};
    return {{View::DumpBasicArea, true}, {View::DumpMlArea, true}};
}

}  // namespace inspect
