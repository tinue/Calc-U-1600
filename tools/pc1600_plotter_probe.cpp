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
#include <string>
#include <vector>

#include "../Core/PC1600/PC1600Machine.hpp"
#include "../Core/Resources/BundledRomCatalog.hpp"
#include "../Core/PC1600/PC1600PresetLoader.hpp"
#include "../Core/Preset/PresetFile.hpp"


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
    std::string romErr;
    if (!BundledRoms::loadPC1600RomSet(machine, {"roms"}, "new", &romErr)) {
        std::fprintf(stderr, "could not load roms/ ROM set (run from repo root): %s\n", romErr.c_str());
        return 1;
    }
    PC1600Machine* mp = &machine;
    PresetLoadResult res = applyPC1600Preset(
        machine, preset,
        [mp](const std::string& l) {
            std::fprintf(stderr, "[preset] %s\n", l.c_str());
            if (mp->ce1600pAttached()) {
                for (const auto& e : mp->drainCE1600PEvents())
                    std::fprintf(stderr, "    <evt> %s\n", e.c_str());
            }
        },
        ".", ".", {}, {"roms"});
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
