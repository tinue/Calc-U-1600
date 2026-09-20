// Headless PC-1600 + CE-1600P plotter probe. Boots a `plotter: ce1600p`
// preset, runs its script, and dumps every pen/motor/colour event the
// AlpsPlotterMechanism logged plus the final turret colour -- so the
// colour-turret behaviour (a `COLOR n` command, the power-on home spin)
// can be inspected without the GUI.
//
// Usage: pc1600_plotter_probe <preset.pc1600> [maxSettleCycles]
//
// ROM set is read from roms/ (same repo-root convention as tools/run_tests.sh).

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../Core/PC1600/PC1600Machine.hpp"
#include "../Core/PC1600/PC1600PresetLoader.hpp"
#include "../Core/PC1500/PresetFile.hpp"

namespace {
bool readRomFile(const char* path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    *out = std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return !out->empty();
}
bool loadRomSet(PC1600Machine& m) {
    std::vector<uint8_t> i0, ii0, iii3, r3b, iv6, r1500;
    if (!readRomFile("roms/PC1600-P0-B0-new.bin", &i0) ||
        !readRomFile("roms/PC1600-P1-B0-new.bin", &ii0) ||
        !readRomFile("roms/PC1600-P1-B3-new.bin", &iii3) ||
        !readRomFile("roms/PC1600-P1-B3B-new.bin", &r3b) ||
        !readRomFile("roms/PC1600-P2-B6-new.bin", &iv6) ||
        !readRomFile("roms/PC1600-LH5803-C000-FFFF-new.bin", &r1500)) {
        return false;
    }
    return m.loadBank0(i0.data(), i0.size(), ii0.data(), ii0.size()) &&
           m.loadBank3Rom(iii3.data(), iii3.size()) &&
           m.loadBank3bRom(r3b.data(), r3b.size()) &&
           m.loadBank6Rom(iv6.data(), iv6.size()) &&
           m.loadLH5803Rom(r1500.data(), r1500.size());
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <preset.pc1600>\n", argv[0]);
        return 1;
    }
    PresetFile preset;
    std::string err;
    if (!parsePresetFile(argv[1], &preset, &err)) {
        std::fprintf(stderr, "parse failed: %s\n", err.c_str());
        return 1;
    }
    PC1600Machine machine;
    if (!loadRomSet(machine)) {
        std::fprintf(stderr, "could not load roms/ ROM set (run from repo root)\n");
        return 1;
    }
    PC1600Machine* mp = &machine;
    PC1600PresetLoadResult res = applyPC1600Preset(
        machine, preset,
        [mp](const std::string& l) {
            std::fprintf(stderr, "[preset] %s\n", l.c_str());
            if (mp->ce1600pAttached()) {
                for (const auto& e : mp->drainCE1600PEvents())
                    std::fprintf(stderr, "    <evt> %s\n", e.c_str());
            }
        },
        ".", ".", {},
        "roms/PC1600-P1-B4-CE1600P.bin", "roms/PC1600-P1-B5-CE1600P-OR-F.bin", "roms/CE-150.ROM");
    if (!res.ok) {
        std::fprintf(stderr, "preset apply failed: %s\n", res.error.c_str());
        return 1;
    }

    if (!machine.ce1600pAttached()) {
        std::fprintf(stderr, "no CE-1600P attached by this preset\n");
        return 1;
    }
    auto events = machine.drainCE1600PEvents();
    std::printf("CE-1600P events (%zu):\n", events.size());
    for (const auto& e : events) std::printf("  %s\n", e.c_str());
    auto pts = machine.ce1600pPlotPoints();
    std::printf("plot points=%zu\n", pts.size());
    if (!pts.empty()) {
        std::printf("first point colour index=%d, last point colour index=%d\n",
                    (int)pts.front().color, (int)pts.back().color);
    }
    return 0;
}
