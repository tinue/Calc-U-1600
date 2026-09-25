#include "Disassembly.hpp"

#include <cstdio>

namespace disasm {

std::string hex8(uint8_t v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "0x%02X", v);
    return buf;
}

std::string hex16(uint16_t v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "0x%04X", v);
    return buf;
}

std::string addrText(uint16_t addr, const SymbolFn& symbols) {
    if (symbols) {
        std::string name = symbols(addr);
        if (!name.empty()) return name;
    }
    return hex16(addr);
}

} // namespace disasm
