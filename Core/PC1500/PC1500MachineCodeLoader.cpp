#include "PC1500MachineCodeLoader.hpp"

#include <cstdio>

#include "PC1500Machine.hpp"

bool loadPC1500MachineCode(PC1500Machine& machine, uint32_t addr, const uint8_t* data, size_t len,
                           std::string* error) {
    if (static_cast<uint64_t>(addr) + len > 0x10000) {
        char b[96];
        std::snprintf(b, sizeof(b), "&%X + %zu bytes runs past &FFFF", addr, len);
        *error = b;
        return false;
    }
    bool allStored = true;
    uint16_t firstMiss = 0;
    for (size_t i = 0; i < len; i++) {
        const uint16_t at = static_cast<uint16_t>(addr + i);
        if (!machine.memory().poke(at, data[i]) && allStored) {
            allStored = false;
            firstMiss = at;
        }
    }
    if (allStored) return true;
    char b[160];
    std::snprintf(b, sizeof(b), "&%X is not RAM (ROM or no memory there) -- the code at &%X-&%X did not load",
                  firstMiss, addr, static_cast<unsigned>(addr + len - 1));
    *error = b;
    return false;
}

void pc1500UserRam(PC1500Machine& machine, uint32_t* start, uint32_t* end) {
    *start = static_cast<uint32_t>(machine.memory().peek(kPc1500RamStPage)) << 8;
    *end = static_cast<uint32_t>(machine.memory().peek(kPc1500RamEndPage)) << 8;
}
