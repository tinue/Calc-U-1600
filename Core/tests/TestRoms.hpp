#pragma once
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../PC1600/PC1600BasicTyper.hpp"
#include "../PC1600/PC1600Machine.hpp"
#include "../Resources/BundledRomCatalog.hpp"

// ROM loading for the ROM-gated tests. Run from the repo root (tools/
// run_tests.sh): the images live in roms/. Every helper returns false when
// an image is missing, so a test can print SKIP instead of failing.

// One ROM image, whole file. Size is left to the loader it's handed to
// (loadBank0/loadBank3Rom/... reject a wrong-size image).
inline bool readRomImage(const char* path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    *out = std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return !out->empty();
}

// The PC-1600's six-file ROM set, through the same loader the app uses.
inline bool loadPC1600Roms(PC1600Machine& m) {
    std::string error;
    return BundledRoms::loadPC1600RomSet(m, {"roms"}, "new", &error);
}

// ROMs loaded, ALL RESET, then the boot run to the prompt -- the same
// sequence the GUI's Reset All and the preset loader use. Attach modules
// or a plotter before calling, so the boot ROM sees them.
inline bool bootPC1600(PC1600Machine& m) {
    if (!loadPC1600Roms(m)) return false;
    m.allReset();
    runBootToPrompt(m);
    return true;
}
