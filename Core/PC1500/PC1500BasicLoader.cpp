#include "PC1500BasicLoader.hpp"

#include <cstdio>

#include "../Basic/BasicBinaryImage.hpp"
#include "PC1500Machine.hpp"

namespace {

// BASIC pointers (big-endian in system RAM).
constexpr uint16_t kBasPrgSt = 0x7865;
constexpr uint16_t kBasPrgEnd = 0x7867;
constexpr uint16_t kVarStart = 0x7899;
constexpr uint16_t kRamStPage = 0x7863;   // high byte of the first valid user-RAM page
constexpr uint16_t kRamEndPage = 0x7864;  // high byte of the first invalid page

uint16_t readBE16(PC1500Machine& m, uint16_t addr) {
    return static_cast<uint16_t>((m.memory().peek(addr) << 8) |
                                 m.memory().peek(static_cast<uint16_t>(addr + 1)));
}

void writeBE16(PC1500Machine& m, uint16_t addr, uint16_t value) {
    m.memory().poke(addr, static_cast<uint8_t>(value >> 8));
    m.memory().poke(static_cast<uint16_t>(addr + 1), static_cast<uint8_t>(value & 0xFF));
}

PC1500BasicLoadResult fail(const std::string& msg) {
    PC1500BasicLoadResult r;
    r.ok = false;
    r.error = msg;
    return r;
}

}  // namespace

PC1500BasicLoadResult loadBasicBinaryProgram(PC1500Machine& machine,
                                             const std::vector<uint8_t>& transferFile) {
    basic::BasicBinaryImage img = basic::parseBasicBinaryTransfer(transferFile);
    if (!img.ok) return fail(img.error);
    if (img.model != basic::TransferModel::PC1500) {
        return fail("this is a PC-1600 tokenized-BASIC transfer file -- load it in a PC-1600 preset");
    }
    return loadBasicBinaryPayload(machine, img.payload);
}

PC1500BasicLoadResult loadBasicBinaryPayload(PC1500Machine& machine,
                                             const std::vector<uint8_t>& payload) {
    // The preset is responsible for putting the machine in a clean, loadable
    // state first (`- key: cl` / `- type: NEW0`) -- exactly as it already is
    // for `format: basic-text`. NEW0 is what initialises BASPRG_ST /
    // VAR_START (both $FFFF after a bare boot) and $FF-fills the program area;
    // this loader just writes into that.
    uint16_t base = readBE16(machine, kBasPrgSt);
    uint16_t ramStart = static_cast<uint16_t>(machine.memory().peek(kRamStPage)) << 8;
    uint32_t ramTop = static_cast<uint32_t>(machine.memory().peek(kRamEndPage)) << 8;

    // After NEW0 the ROM sets BASPRG_ST to (RAM start page):C5 -- the $C5
    // reserve area always sits at the bottom of user RAM, whatever page that
    // is: $00C5 with a 16K card in the low window, $40C5 on a stock
    // PC-1500A, $C0C5 on an unexpanded PC-1500, and so on. So a valid base
    // lies inside [RAM start, RAM end); an uninitialised pointer -- the
    // classic "forgot NEW0" tell is a $00 low byte ($0000), or $FFFF after a
    // bare boot -- does not.
    if (base < ramStart || base >= ramTop || (base & 0x00FF) == 0x00) {
        char buf[176];
        std::snprintf(buf, sizeof(buf),
                      "BASPRG_ST is $%04X, outside user RAM $%04X-$%04X -- run NEW0 before the "
                      "program block (a `- key: cl` / `- type: NEW0` keys section)",
                      base, ramStart, static_cast<unsigned>(ramTop));
        return fail(buf);
    }

    uint32_t end = base + static_cast<uint32_t>(payload.size());  // where the 0xFF marker goes
    uint16_t varStart = readBE16(machine, kVarStart);
    uint32_t ceiling = (varStart > base && varStart <= ramTop) ? varStart : ramTop;
    if (end + 1 > ceiling) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "program too large: needs %zu bytes from $%04X, only %u free below $%04X",
                      payload.size() + 1, base, base < ceiling ? ceiling - base : 0u, ceiling);
        return fail(buf);
    }

    uint16_t addr = base;
    for (uint8_t b : payload) machine.memory().poke(addr++, b);
    machine.memory().poke(addr, 0xFF);  // program-end marker

    writeBE16(machine, kBasPrgEnd, static_cast<uint16_t>(end));

    PC1500BasicLoadResult r;
    r.ok = true;
    r.baseAddr = base;
    r.endAddr = static_cast<uint16_t>(end);
    return r;
}
