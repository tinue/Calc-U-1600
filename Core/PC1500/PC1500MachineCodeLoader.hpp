#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

class PC1500Machine;

// Pokes a machine-language block into the PC-1500/1500A at `addr`, the
// same memory().poke() path the preset loader's `format: binary` uses. A
// byte poke() reports as not stored (ROM, open bus, a card that drops it)
// means the range isn't all RAM (`error` says where). Not GUI-
// thread-safe on its own -- the caller must hold the machine still (the
// GUI runs it inside runSynchronousLoad(), frame timer stopped).
bool loadPC1500MachineCode(PC1500Machine& machine, uint32_t addr, const uint8_t* data, size_t len,
                           std::string* error);

// The ROM's user-RAM bounds: RAM_ST / RAM_END hold page numbers (the high
// byte of the first valid / first invalid page).
constexpr uint16_t kPc1500RamStPage = 0x7863;
constexpr uint16_t kPc1500RamEndPage = 0x7864;

// User RAM as the ROM reports it: `start` = RAM_ST * 256, `end` = RAM_END *
// 256 (one past the last byte). Unlocked reads, like the loader above.
void pc1500UserRam(PC1500Machine& machine, uint32_t* start, uint32_t* end);
