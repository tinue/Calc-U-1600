// Headless C++ tests for the LH5803 core (a thin
// subclass of LH5801, see Core/CPU/LH5803/LH5803.hpp) and its standalone
// LH5803Memory bus adapter. Same no-framework, assert-and-tally style as
// lh5801_tests.cpp -- see that file's header comment.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <vector>

#include "../CPU/LH5803/LH5803.hpp"
#include "../CPU/LH5803/LH5803Memory.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

void test_lh5803_inherits_lh5801_reset_and_opcode_behavior() {
    // Sanity check that LH5803 really is LH5801 underneath -- same reset
    // vector convention (16-bit big-endian at ME0 0xFFFE/0xFFFF) and same
    // opcode execution (LDI A,n), exercised through the LH5803 type. The
    // reset vector itself lives in ROM (0xFFFE/0xFFFF fall in the
    // C000-FFFF ROM window, which ignores poke()), so it has to be baked
    // into a loaded ROM image rather than poked in; the vector points at
    // 0x4234, inside the RAM window, so the LDI that follows it can still
    // be poked in directly.
    LH5803Memory mem;
    std::vector<uint8_t> rom(16384, 0x00);
    rom[16384 - 2] = 0x42; // 0xFFFE
    rom[16384 - 1] = 0x34; // 0xFFFF -> reset vector 0x4234
    CHECK(mem.loadROM(rom.data(), rom.size()));
    LH5803 cpu(mem);
    cpu.reset();
    CHECK(cpu.pc() == 0x4234);

    // LDI A,0x7F at the reset vector target.
    mem.poke(0x4234, 0xB5);
    mem.poke(0x4235, 0x7F);
    cpu.step();
    CHECK(cpu.a() == 0x7F);
    CHECK(!cpu.flagZ());
}

void test_memory_slot_window_open_bus() {
    LH5803Memory mem;
    CHECK(mem.peek(0x0000) == 0xFF);
    CHECK(mem.peek(0x3FFF) == 0xFF);
    mem.poke(0x1000, 0x55); // ignored -- open bus
    CHECK(mem.peek(0x1000) == 0xFF);
}

void test_memory_internal_ram_readwrite() {
    LH5803Memory mem;
    mem.poke(0x4000, 0xAB);
    CHECK(mem.peek(0x4000) == 0xAB);
    mem.poke(0x7FFF, 0xCD);
    CHECK(mem.peek(0x7FFF) == 0xCD);
}

void test_memory_ce158_window_open_bus_in_v1() {
    LH5803Memory mem;
    CHECK(mem.peek(0x8000) == 0xFF);
    CHECK(mem.peek(0xBFFF) == 0xFF);
    mem.poke(0x9000, 0x99);
    CHECK(mem.peek(0x9000) == 0xFF);
}

void test_memory_rom_load_and_write_ignored() {
    LH5803Memory mem;
    std::vector<uint8_t> rom(16384, 0x42);
    CHECK(mem.loadROM(rom.data(), rom.size()));
    CHECK(mem.peek(0xC000) == 0x42);
    CHECK(mem.peek(0xFFFF) == 0x42);
    mem.poke(0xC000, 0x99);
    CHECK(mem.peek(0xC000) == 0x42); // unchanged
}

void test_memory_load_rejects_wrong_size() {
    LH5803Memory mem;
    std::vector<uint8_t> tooShort(100, 0);
    CHECK(!mem.loadROM(tooShort.data(), tooShort.size()));
}

void test_reset_clears_ram_not_rom() {
    LH5803Memory mem;
    std::vector<uint8_t> rom(16384, 0x42);
    mem.loadROM(rom.data(), rom.size());
    mem.poke(0x4000, 0xAB);
    mem.reset();
    CHECK(mem.peek(0x4000) == 0x00); // RAM powers up 0x00, see .cpp comment
    CHECK(mem.peek(0xC000) == 0x42); // ROM untouched
}

void test_boot_smoke_real_rom() {
    LH5803Memory mem;
    if (!mem.loadROMFile("roms/PC1600-LH5803-C000-FFFF.bin")) {
        std::fprintf(stderr, "SKIP test_boot_smoke_real_rom: roms/PC1600-LH5803-C000-FFFF.bin "
                              "not found relative to cwd (run tests from the repo root)\n");
        return;
    }
    LH5803 cpu(mem);
    cpu.reset();
    CHECK(cpu.pc() == 0xE000); // confirmed reset vector target (matches the real PC-1500 A04 ROM's own)

    // Real, meaningful signal that ROM execution is correct so far: the
    // boot sequence reaches and repeatedly executes a delay loop at
    // 0xE006 (visible by tracing the first ~60 steps) before falling
    // through further into the boot sequence.
    bool sawDelayLoop = false;
    int delayLoopHits = 0;
    for (int i = 0; i < 300 && delayLoopHits < 20; i++) {
        if (cpu.pc() == 0xE006) { sawDelayLoop = true; delayLoopHits++; }
        cpu.step();
    }
    CHECK(sawDelayLoop);
    CHECK(delayLoopHits >= 20);

    // NOT asserted here: convergence to a stable BASIC-idle loop. Tracing
    // past the delay loop shows execution falls through into the
    // 0000-3FFF "Slot 1/2 module RAM" window (per
    // PC-1600-CPU-LH5803-Compat.md §2) almost immediately afterward --
    // which this standalone LH5803Memory correctly reports as open bus,
    // since no concrete memory module is attached here and, more
    // fundamentally, that window is supposed to be backed by whatever the
    // SC7852 side has bank-switched into Slot 1/2 (shared physical RAM,
    // not LH5803-private storage) -- whether the LH5803 alone can ever
    // reach a stable idle loop without that real data is unclear;
    // full end-to-end convergence is deferred to Phase 5.3, once
    // PC1600Machine wires the two CPUs and their shared RAM together for
    // real.
}

} // namespace

int run_lh5803_tests() {
    test_lh5803_inherits_lh5801_reset_and_opcode_behavior();
    test_memory_slot_window_open_bus();
    test_memory_internal_ram_readwrite();
    test_memory_ce158_window_open_bus_in_v1();
    test_memory_rom_load_and_write_ignored();
    test_memory_load_rejects_wrong_size();
    test_reset_clears_ram_not_rom();
    test_boot_smoke_real_rom();

    std::printf("lh5803_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
