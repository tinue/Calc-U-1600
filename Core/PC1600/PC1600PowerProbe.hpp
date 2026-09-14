#pragma once
//
// Throw-away instrumentation for tracing what the ROM does when OFF is
// pressed. Compiled in ONLY when PC1600_POWER_PROBE is defined --
// tools/build_pc1600_power_probe.sh
// defines it; the normal build and tools/run_tests.sh do not, so this is
// exactly zero code in every shipping path.
//
// The harness (tools/pc1600_power_probe.cpp) updates g_pc before each
// machine step() and flips g_active on once the machine has settled and the
// OFF key has been pressed. The hooks below are called from
// PC1600Memory::writeIO / PC1600SubCpu::command behind the same #ifdef.
//
#ifdef PC1600_POWER_PROBE

#include <cstdint>
#include <cstdio>

namespace pc1600probe {

inline uint16_t g_pc = 0;        // PC of the instruction currently executing
inline bool     g_active = false; // set true by the harness after OFF is pressed
inline bool     g_ownsSc7852 = true;
inline bool     g_verbose = true; // harness lowers this during the steady-state profile
inline uint64_t g_ioWriteCount[256] = {};
inline uint64_t g_subCmdCount[256] = {};

inline void onIoWrite(uint8_t port, uint8_t value) {
    if (!g_active) return;
    g_ioWriteCount[port]++;
    if (g_verbose)
        std::fprintf(stderr, "  [%s @%04X]  OUT (%02X),%02X\n",
                     g_ownsSc7852 ? "Z80 " : "5803", g_pc, port, value);
}

inline void onIoRead(uint8_t port, uint8_t value) {
    if (!g_active) return;
    // Only the interesting ports, or this drowns the log: the TC8576F UART
    // register file (20-27H), the sub-CPU answer (33H) and the rest of the
    // 30-3FH control block.
    if ((port >= 0x20 && port <= 0x27) || (port >= 0x30 && port <= 0x3F)) {
        std::fprintf(stderr, "  [%s @%04X]  IN  A,(%02X) -> %02X\n",
                     g_ownsSc7852 ? "Z80 " : "5803", g_pc, port, value);
    }
}

// Which branch of PC1600Memory::read() served a page-C address -- the
// question "does the ROM's medium probe at IX=8000h actually reach the
// CE-1601M?" is exactly this tag. Set g_slotWatch to enable.
inline bool     g_slotWatch = false;
inline uint16_t g_slotWatchLo = 0x8000, g_slotWatchHi = 0x8010;
inline const char* kReadSource[] = {"slot2-remapped", "rom/ram", "slot1",
                                    "slot2-direct", "OPEN-BUS", "slot1-remapped"};

inline void onSlotRead(uint16_t addr, uint8_t value, int source,
                       uint8_t port31, uint8_t port28, uint8_t port3c) {
    if (!g_active || !g_slotWatch) return;
    if (addr < g_slotWatchLo || addr > g_slotWatchHi) return;
    std::fprintf(stderr,
                 "  [MEM @%04X] read %04X -> %02X  via %-14s "
                 "(31=%02X pageC=%u  28=%02X  3C=%02X)\n",
                 g_pc, addr, value, kReadSource[source], port31,
                 (unsigned)((port31 >> 4) & 0x07), port28, port3c);
}

// Same question for the write side: does INIT ever try to write the medium,
// and does the write land on the card or get dropped? `claimed` is which
// branch of writeImpl() took it (see kReadSource; 4 == nothing took it).
inline bool g_writeWatchAll = false;   // true: every write, not just the window
inline bool g_cardWritesOnly = false;  // true: only writes a slot card actually took
inline void onSlotWrite(uint16_t addr, uint8_t value, int claimed,
                        uint8_t port31, uint8_t port28) {
    if (!g_active || !g_slotWatch) return;
    if (g_cardWritesOnly && claimed != 0 && claimed != 2 && claimed != 3 && claimed != 5) return;
    if (!g_writeWatchAll && (addr < g_slotWatchLo || addr > g_slotWatchHi)) return;
    std::fprintf(stderr,
                 "  [MEM @%04X] WRITE %04X <- %02X  %-14s (31=%02X pageC=%u 28=%02X)\n",
                 g_pc, addr, value, kReadSource[claimed], port31,
                 (unsigned)((port31 >> 4) & 0x07), port28);
}

inline void onSubCpuCommand(uint8_t cmd) {
    if (!g_active) return;
    g_subCmdCount[cmd]++;
    if (g_verbose)
        std::fprintf(stderr, "  [%s @%04X]  sub-CPU cmd %02X\n",
                     g_ownsSc7852 ? "Z80 " : "5803", g_pc, cmd);
}

} // namespace pc1600probe

#endif // PC1600_POWER_PROBE
