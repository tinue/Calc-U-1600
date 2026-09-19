#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../PC1500/PC1500Machine.hpp"
#include "../PC1600/PC1600Machine.hpp"

// ── Catalogue of bundled ROM images ────────────────────────────────────
//
// Mirrors Core/Connector/MemoryCardCatalog.hpp's own shape and reasoning:
// no notion of "app resource" (a Qt qrc alias, a repo-relative CLI
// convention) reaches Core here either -- the caller supplies a list of
// directories to search (the GUI's bundled resources folder, the CLI's
// `roms/`), and Core resolves a fixed, internally-known filename and does
// its own file I/O. A preset (or a manual "attach CE-150" button) names
// WHAT it wants ("ce150", "A04", ...); Core alone knows WHERE that lives
// on disk and how many bytes it should be.
namespace BundledRoms {

namespace detail {

inline bool readWholeFile(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// Bundled ROM bytes never change during a run, but switching models or
// re-arming a preset re-resolves and re-reads every ROM file involved.
// Cache by resolved path so repeated switches don't keep hitting disk.
inline bool readWholeFileCached(const std::string& path, std::vector<uint8_t>* out) {
    static std::map<std::string, std::vector<uint8_t>> cache;
    auto it = cache.find(path);
    if (it != cache.end()) {
        *out = it->second;
        return true;
    }
    std::vector<uint8_t> bytes;
    if (!readWholeFile(path, &bytes)) return false;
    *out = bytes;
    cache.emplace(path, std::move(bytes));
    return true;
}

}  // namespace detail

// Searches `dirs` in order for a file literally named `filename`, returning
// the first match. Unlike MemoryCardCatalog's resolveModuleSpecByName, ROM
// filenames are fixed and internally known (not user-chosen), so there is
// no "declared by more than one file" ambiguity to detect -- first match
// wins, matching moduleDir/extraModuleDirs' own "first dir that has it"
// convention.
inline bool resolveBundledRomPath(const std::vector<std::string>& dirs, const std::string& filename,
                                  std::string* outPath, std::string* error) {
    for (const auto& dir : dirs) {
        if (dir.empty()) continue;
        std::error_code ec;
        std::filesystem::path candidate = std::filesystem::path(dir) / filename;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            if (outPath) *outPath = candidate.string();
            return true;
        }
    }
    if (error) *error = "bundled ROM '" + filename + "' not found in any of the given directories";
    return false;
}

// Loads the PC-1500 firmware ROM revision named by `romVariant`
// ("A01"/"A03"/"A04", see PresetFile::romVariant's doc) into `machine`.
inline bool loadPC1500Rom(PC1500Machine& machine, const std::string& romVariant,
                          const std::vector<std::string>& dirs, std::string* error) {
    std::string path;
    if (!resolveBundledRomPath(dirs, "PC-1500_" + romVariant + ".ROM", &path, error)) return false;
    if (!machine.loadROMFile(path)) {
        if (error) *error = "failed to load firmware ROM: " + path;
        return false;
    }
    return true;
}

// Loads the PC-1600's fixed six-file ROM set into `machine`. Always the
// same six files -- a PC-1600 preset/model never chooses among revisions.
inline bool loadPC1600RomSet(PC1600Machine& machine, const std::vector<std::string>& dirs,
                             std::string* error) {
    auto loadOne = [&](const char* filename, std::vector<uint8_t>* out) {
        std::string path;
        return resolveBundledRomPath(dirs, filename, &path, error) &&
               detail::readWholeFileCached(path, out);
    };
    std::vector<uint8_t> romI, romII, romIII, rom3b, romIV, rom1500;
    struct Entry {
        const char* filename;
        std::vector<uint8_t>* out;
    };
    const Entry entries[] = {
        {"PC1600-P0-B0.bin", &romI},
        {"PC1600-P1-B0.bin", &romII},
        {"PC1600-P1-B3.bin", &romIII},
        {"PC1600-P1-B3B.bin", &rom3b},
        {"PC1600-P2-B6.bin", &romIV},
        {"PC1600-LH5803-C000-FFFF.bin", &rom1500},
    };
    for (const auto& entry : entries) {
        if (!loadOne(entry.filename, entry.out)) {
            if (error && error->empty()) *error = "failed to read the PC-1600 ROM set";
            return false;
        }
    }
    machine.loadBank0(romI.data(), romI.size(), romII.data(), romII.size());
    machine.loadBank3Rom(romIII.data(), romIII.size());
    machine.loadBank3bRom(rom3b.data(), rom3b.size());
    machine.loadBank6Rom(romIV.data(), romIV.size());
    machine.loadLH5803Rom(rom1500.data(), rom1500.size());
    return true;
}

// Attaches the CE-150 plotter to `machine` (PC-1600's LH5803 side or a
// PC-1500/1500A) -- both machine types expose the same attachCE150(bytes,
// size) shape, so one template covers both.
template <typename Machine>
inline bool attachCE150(Machine& machine, const std::vector<std::string>& dirs, std::string* error) {
    std::string path;
    std::vector<uint8_t> rom;
    if (!resolveBundledRomPath(dirs, "CE-150.ROM", &path, error) ||
        !detail::readWholeFileCached(path, &rom)) {
        if (error && error->empty()) *error = "could not read the CE-150 ROM";
        return false;
    }
    if (!machine.attachCE150(rom.data(), rom.size())) {
        if (error) *error = "CE-150 attach failed -- ROM size/shape rejected (expected 8192 bytes)";
        return false;
    }
    return true;
}

// Attaches the CE-1600P plotter (and, per its union attach, the CE-1600F
// floppy, with a blank disk -- PC1600Machine::attachCE1600P()) to a
// PC-1600. Load a disk afterwards with PC1600Machine::ce1600fLoadImage().
inline bool attachCE1600P(PC1600Machine& machine, const std::vector<std::string>& dirs,
                          std::string* error) {
    std::string path1, path2;
    std::vector<uint8_t> rom1, rom2;
    if (!resolveBundledRomPath(dirs, "PC1600-P1-B4-CE1600P.bin", &path1, error) ||
        !detail::readWholeFileCached(path1, &rom1) ||
        !resolveBundledRomPath(dirs, "PC1600-P1-B5-CE1600P-OR-F.bin", &path2, error) ||
        !detail::readWholeFileCached(path2, &rom2)) {
        if (error && error->empty()) *error = "could not read the CE-1600P ROM";
        return false;
    }
    if (!machine.attachCE1600P(rom1.data(), rom1.size(), rom2.data(), rom2.size())) {
        if (error) *error = "CE-1600P attach failed -- ROM size/shape rejected";
        return false;
    }
    return true;
}

// Attaches the plotter named by `plotterName` ("ce150"/"ce1600p"/"" for
// none) to a PC-1600 -- the shape a preset's `plotter:` field or the GUI's
// two toggle buttons both want. Returns true and does nothing for "".
inline bool attachPlotterByName(PC1600Machine& machine, const std::string& plotterName,
                                const std::vector<std::string>& dirs, std::string* error,
                                bool* outCe150Attached = nullptr) {
    if (plotterName.empty()) return true;
    if (plotterName == "ce150") {
        if (!attachCE150(machine, dirs, error)) return false;
        if (outCe150Attached) *outCe150Attached = true;
        return true;
    }
    if (plotterName == "ce1600p") return attachCE1600P(machine, dirs, error);
    if (error) *error = "plotter: '" + plotterName + "' is not a known plotter";
    return false;
}

}  // namespace BundledRoms
