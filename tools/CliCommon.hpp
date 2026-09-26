#pragma once
// Small helpers shared by the headless CLIs (pc1500_cli, pc1600_cli) and
// their peers (Ce158CliPeer.hpp): whole-file I/O and the CE-150 summary.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../Core/Resources/BundledRomCatalog.hpp"

namespace cli {

/// Reads the whole file. False if it can't be opened.
inline bool readFile(const std::string& path, std::vector<uint8_t>* out) {
    return BundledRoms::detail::readWholeFile(path, out);
}

/// Reads the whole file and requires exactly `size` bytes (ROM images).
inline bool readFileExact(const std::string& path, size_t size, std::vector<uint8_t>* out) {
    return readFile(path, out) && out->size() == size;
}

/// Writes `text` to `path`, replacing it. False on any open/write/close error.
inline bool writeFile(const std::string& path, const std::string& text) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

/// Prints the CE-150 plot summary (points, revision, first 60 events) when a
/// CE-150 is attached. Works on either machine.
template <typename Machine>
void printCe150Report(Machine& machine) {
    if (!machine.ce150Attached()) return;
    std::printf("CE-150: attached, plot points=%zu revision=%llu\n", machine.ce150PlotPoints().size(),
                static_cast<unsigned long long>(machine.ce150PlotRevision()));
    const auto events = machine.drainCE150Events();
    std::printf("CE-150 events (%zu):\n", events.size());
    for (size_t i = 0; i < events.size() && i < 60; ++i) std::printf("  %s\n", events[i].c_str());
}

}  // namespace cli
