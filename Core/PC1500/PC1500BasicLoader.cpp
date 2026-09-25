#include "PC1500BasicLoader.hpp"

#include <cstdio>

#include "../Basic/BasicBinaryImage.hpp"
#include "PC1500Machine.hpp"
#include "PC1500MachineCodeLoader.hpp"

namespace {

// BASIC pointers (big-endian in system RAM).
constexpr uint16_t kBasPrgSt = 0x7865;
constexpr uint16_t kBasPrgEnd = 0x7867;
constexpr uint16_t kVarStart = 0x7899;

uint16_t readBE16(PC1500Machine& m, uint16_t addr) {
    return static_cast<uint16_t>((m.memory().peek(addr) << 8) |
                                 m.memory().peek(static_cast<uint16_t>(addr + 1)));
}

void writeBE16(PC1500Machine& m, uint16_t addr, uint16_t value) {
    m.memory().poke(addr, static_cast<uint8_t>(value >> 8));
    m.memory().poke(static_cast<uint16_t>(addr + 1), static_cast<uint8_t>(value & 0xFF));
}

BasicLoadResult fail(const std::string& msg) {
    BasicLoadResult r;
    r.ok = false;
    r.error = msg;
    return r;
}

}  // namespace

BasicLoadResult loadBasicBinaryProgram(PC1500Machine& machine,
                                       const std::vector<uint8_t>& transferFile) {
    basic::BasicBinaryImage img = basic::parseBasicBinaryTransfer(transferFile);
    if (!img.ok) return fail(img.error);
    if (img.model != basic::TransferModel::PC1500) {
        return fail("this is a PC-1600 tokenized-BASIC transfer file -- load it in a PC-1600 preset");
    }
    return loadBasicBinaryPayload(machine, img.payload);
}

BasicLoadResult loadBasicBinaryPayload(PC1500Machine& machine,
                                       const std::vector<uint8_t>& payload) {
    // LOAD semantics: this works off whatever BASPRG_ST/BASPRG_END are
    // currently live -- there is no NEW0 precondition. The caller (the user,
    // via the menu, or a preset's own `- type: NEW0` step) is responsible for
    // having the machine in a state where these pointers are valid; this
    // loader validates them, erases the resident program between them, pokes
    // the new payload in from BASPRG_ST, and fixes up BASPRG_END.
    uint16_t base = readBE16(machine, kBasPrgSt);
    uint32_t ramStart = 0, ramTop = 0;
    pc1500UserRam(machine, &ramStart, &ramTop);

    // BASPRG_ST always lands in the $C5 reserve area at the bottom of
    // whatever user RAM is currently mapped in -- $00C5 with a 16K card in
    // the low window, $40C5 on a stock PC-1500A, $C0C5 on an unexpanded
    // PC-1500, $0112 after a `NEW &112`, and so on: any low byte other than
    // $00 is plausible. So a valid base lies inside [RAM start, RAM end); an
    // uninitialised pointer -- a $00 low byte ($0000), or $FFFF after a bare
    // boot -- does not.
    if (base < ramStart || base >= ramTop || (base & 0x00FF) == 0x00) {
        char buf[176];
        std::snprintf(buf, sizeof(buf),
                      "BASPRG_ST is $%04X, outside user RAM $%04X-$%04X -- run NEW before loading",
                      base, static_cast<unsigned>(ramStart), static_cast<unsigned>(ramTop));
        return fail(buf);
    }

    uint16_t oldEnd = readBE16(machine, kBasPrgEnd);
    if (oldEnd < base || oldEnd >= ramTop || (oldEnd & 0x00FF) == 0x00) {
        char buf[176];
        std::snprintf(buf, sizeof(buf),
                      "BASPRG_END is $%04X, outside/invalid for BASPRG_ST $%04X -- run NEW before "
                      "loading",
                      oldEnd, base);
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

    // Erase the resident program (whatever was there before) so a shorter
    // reload doesn't leave stale tokens dangling past the new BASPRG_END.
    for (uint16_t addr = base; addr <= oldEnd; ++addr) machine.memory().poke(addr, 0x00);

    uint16_t addr = base;
    for (uint8_t b : payload) machine.memory().poke(addr++, b);
    machine.memory().poke(addr, 0xFF);  // program-end marker

    writeBE16(machine, kBasPrgEnd, static_cast<uint16_t>(end));

    BasicLoadResult r;
    r.ok = true;
    r.baseAddr = base;
    r.endAddr = static_cast<uint16_t>(end);
    return r;
}
