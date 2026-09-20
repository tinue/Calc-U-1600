// Throw-away probe: trace the ROM path the PC-1600 takes when the OFF key
// is pressed.
//
// Boots the full dual-CPU PC1600Machine on the confirmed ROM set, runs to
// the post-boot idle loop, presses OFF, then streams:
//   * every OUT (nn),v and sub-CPU command byte (via PC1600PowerProbe.hpp)
//   * every *newly reached* PC (a novelty filter drops loop spam)
//   * where/if either CPU HALTs
//
// Build: tools/build_pc1600_power_probe.sh
// Run:   ./pc1600_power_probe [roms-dir]   (default ./roms)

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "../Core/CPU/SC7852/SC7852.hpp"
#include "../Core/PC1600/PC1600Machine.hpp"
#include "../Core/PC1600/PC1600PowerProbe.hpp"

namespace {

bool readRom(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    *out = std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    return out->size() == 16384;
}

uint32_t key(const PC1600Machine& m) {
    const bool z80 = m.sc7852Owns();
    const uint16_t pc = z80 ? const_cast<PC1600Machine&>(m).sc7852().pc()
                            : const_cast<PC1600Machine&>(m).lh5803().pc();
    return (z80 ? 0u : 0x10000u) | pc;
}

void printKey(uint32_t k) {
    std::fprintf(stderr, "%s %04X", (k & 0x10000) ? "5803" : "Z80 ", k & 0xFFFF);
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc >= 2) ? std::string(argv[1]) + "/" : "roms/";

    std::vector<uint8_t> i0, ii0, iii3, r3b, iv6, r1500;
    const bool ok =
        readRom(dir + "PC1600-P0-B0-new.bin", &i0) &&
        readRom(dir + "PC1600-P1-B0-new.bin", &ii0) &&
        readRom(dir + "PC1600-P1-B3-new.bin", &iii3) &&
        readRom(dir + "PC1600-P1-B3B-new.bin", &r3b) &&
        readRom(dir + "PC1600-P2-B6-new.bin", &iv6) &&
        readRom(dir + "PC1600-LH5803-C000-FFFF-new.bin", &r1500);
    if (!ok) {
        std::fprintf(stderr, "could not read the PC1600-*.bin set from '%s'\n", dir.c_str());
        return 1;
    }

    PC1600Machine m;
    if (!m.loadBank0(i0.data(), i0.size(), ii0.data(), ii0.size()) ||
        !m.loadBank3Rom(iii3.data(), iii3.size()) ||
        !m.loadBank3bRom(r3b.data(), r3b.size()) ||
        !m.loadBank6Rom(iv6.data(), iv6.size()) ||
        !m.loadLH5803Rom(r1500.data(), r1500.size())) {
        std::fprintf(stderr, "ROM load failed\n");
        return 1;
    }
    m.allReset();
    m.seedClock(2026, 9, 2, 12, 0, 0);

    uint64_t tstates = 0;
    uint64_t steps = 0;
    auto stepOnce = [&]() {
        const bool z80 = m.sc7852Owns();
        pc1600probe::g_ownsSc7852 = z80;
        pc1600probe::g_pc = z80 ? m.sc7852().pc() : static_cast<uint16_t>(m.lh5803().pc());
        const int c = m.step();
        const uint64_t cyc = static_cast<uint64_t>(c > 0 ? c : SC7852::kHaltTickCycles);
        tstates += PC1600Machine::toTStates(cyc, z80);
        steps++;
    };

    // ── Boot settle ─────────────────────────────────────────────────────
    const uint64_t kSettle = static_cast<uint64_t>(PC1600Machine::kTStateHz) * 4;
    while (tstates < kSettle) stepOnce();
    std::fprintf(stderr, "boot settle: %llu steps, %llu T-states\n",
                 (unsigned long long)steps, (unsigned long long)tstates);

    // ── Characterise the idle loop ─────────────────────────────────────
    std::unordered_map<uint32_t, uint64_t> idle;
    for (int i = 0; i < 400000; i++) { idle[key(m)]++; stepOnce(); }
    std::fprintf(stderr, "\nidle-loop PC profile (top 20 of %zu distinct):\n", idle.size());
    {
        std::vector<std::pair<uint32_t, uint64_t>> v(idle.begin(), idle.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < v.size() && i < 20; i++) {
            std::fprintf(stderr, "  ");
            printKey(v[i].first);
            std::fprintf(stderr, "  x%llu\n", (unsigned long long)v[i].second);
        }
    }

    // ── Press OFF, trace the divergence ────────────────────────────────
    std::fprintf(stderr, "\n=== OFF pressed (t=%llu) ===\n", (unsigned long long)tstates);
    pc1600probe::g_active = true;
    pc1600probe::g_verbose = true;
    m.pressKey("off");

    std::unordered_map<uint32_t, uint64_t> lastSeen; // key -> step last printed
    const uint64_t kBudget = static_cast<uint64_t>(PC1600Machine::kTStateHz) * 3;
    const uint64_t tStart = tstates;
    bool released = false;
    int firstHaltReported = 0;

    while (tstates - tStart < kBudget) {
        if (!released && tstates - tStart > PC1600Machine::kTStateHz / 2) {
            m.releaseKey("off");
            released = true;
            std::fprintf(stderr, "  (OFF released, t=%llu)\n", (unsigned long long)tstates);
        }
        const uint32_t k = key(m);
        auto it = lastSeen.find(k);
        if (it == lastSeen.end() || steps - it->second > 20000) {
            std::fprintf(stderr, "  path  ");
            printKey(k);
            std::fprintf(stderr, "   (t=+%llu)\n", (unsigned long long)(tstates - tStart));
        }
        lastSeen[k] = steps;

        stepOnce();

        if (!firstHaltReported && m.sc7852().halted() && (tstates - tStart) > 1000) {
            std::fprintf(stderr, "  [SC7852 re-entered HALT at PC=%04X, t=+%llu]\n",
                         m.sc7852().pc(), (unsigned long long)(tstates - tStart));
            firstHaltReported = 1;
        }
    }

    std::fprintf(stderr, "\n=== after OFF ===\n");
    std::fprintf(stderr, "bus owner: %s   SC7852 PC=%04X halted=%d   LH5803 PC=%04X halted=%d\n",
                 m.sc7852Owns() ? "SC7852" : "LH5803",
                 m.sc7852().pc(), m.sc7852().halted() ? 1 : 0,
                 static_cast<uint16_t>(m.lh5803().pc()), m.lh5803().halted() ? 1 : 0);

    // Post-OFF steady-state PC profile (to catch a "wait for ON" spin loop
    // that never HALTs). Quiet the per-write spam but keep counting.
    pc1600probe::g_verbose = false;
    std::unordered_map<uint32_t, uint64_t> after;
    for (int i = 0; i < 400000; i++) { after[key(m)]++; stepOnce(); }
    std::fprintf(stderr, "\npost-OFF PC profile (top 20 of %zu distinct; '*' = also in idle loop):\n",
                 after.size());
    {
        std::vector<std::pair<uint32_t, uint64_t>> v(after.begin(), after.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < v.size() && i < 20; i++) {
            std::fprintf(stderr, "  %s", idle.count(v[i].first) ? "*" : " ");
            printKey(v[i].first);
            std::fprintf(stderr, "  x%llu\n", (unsigned long long)v[i].second);
        }
    }

    std::fprintf(stderr, "\nI/O writes seen after OFF (port: count):\n");
    for (int p = 0; p < 256; p++)
        if (pc1600probe::g_ioWriteCount[p])
            std::fprintf(stderr, "  OUT (%02X)  x%llu\n", p,
                         (unsigned long long)pc1600probe::g_ioWriteCount[p]);
    std::fprintf(stderr, "sub-CPU commands seen after OFF (cmd: count):\n");
    for (int c = 0; c < 256; c++)
        if (pc1600probe::g_subCmdCount[c])
            std::fprintf(stderr, "  cmd %02X  x%llu\n", c,
                         (unsigned long long)pc1600probe::g_subCmdCount[c]);
    return 0;
}
