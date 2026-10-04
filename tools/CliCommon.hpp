#pragma once
// Small helpers shared by the headless CLIs (pc1500_cli, pc1600_cli) and
// their peers (Ce158CliPeer.hpp): whole-file I/O, the CE-150 summary, the
// cassette options and memory dumps.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../Core/FileIO.hpp"
#include "../Core/Resources/BundledRomCatalog.hpp"

namespace cli {

/// Reads the whole file. False if it can't be opened.
inline bool readFile(const std::string& path, std::vector<uint8_t>* out) {
    return readWholeFile(path, out);
}

/// Reads the whole file and requires exactly `size` bytes (ROM images).
inline bool readFileExact(const std::string& path, size_t size, std::vector<uint8_t>* out) {
    return readFile(path, out) && out->size() == size;
}

/// Writes `text` to `path`, replacing it. False on any open/write/close error.
inline bool writeFile(const std::string& path, const void* data, size_t size) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, size, f) == size;
    return std::fclose(f) == 0 && ok;
}
inline bool writeFile(const std::string& path, const std::string& text) {
    return writeFile(path, text.data(), text.size());
}
inline bool writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    return writeFile(path, bytes.data(), bytes.size());
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

/// --tape-in <in.wav> / --tape-out <out.wav>: a cassette for the recorder
/// behind the tape interface, put in before the run and taken out after it.
struct TapeOptions {
    std::string in, out;

    bool parseArg(int argc, char** argv, int& i) {
        if (i + 1 >= argc) return false;
        if (std::strcmp(argv[i], "--tape-in") == 0) { in = argv[++i]; return true; }
        if (std::strcmp(argv[i], "--tape-out") == 0) { out = argv[++i]; return true; }
        return false;
    }

    template <typename Machine>
    bool arm(Machine& machine) const {
        if (!in.empty()) {
            std::string error;
            if (!machine.tapePlay(in, error)) {
                std::fprintf(stderr, "--tape-in: %s\n", error.c_str());
                return false;
            }
        } else if (!out.empty()) {
            machine.tapeRecord(out);
        }
        return true;
    }

    /// Reports the tape position and ejects it, saving a recording.
    template <typename Machine>
    bool finish(Machine& machine) const {
        if (in.empty() && out.empty()) return true;
        const auto tape = machine.tapeStatus();
        std::printf("Tape: %.2f s of %.2f s, motor %s\n", tape.position, tape.length, tape.motor ? "on" : "off");
        std::string error;
        if (!machine.tapeEject(&error)) {
            std::fprintf(stderr, "--tape-out: %s\n", error.c_str());
            return false;
        }
        if (!out.empty()) std::printf("Wrote %.2f s of tape to %s\n", tape.length, out.c_str());
        return true;
    }
};

/// --dump-mem <addr>,<len> (repeatable): memory printed as hex at the end.
using MemDumps = std::vector<std::pair<uint32_t, uint32_t>>;

/// Adds `<addr>,<len>` to `dumps`; false (with a message) if malformed.
inline bool parseMemDump(const char* arg, MemDumps* dumps) {
    char* rest = nullptr;
    const auto start = static_cast<uint32_t>(std::strtoul(arg, &rest, 0));
    const uint32_t length = (rest && *rest == ',') ? static_cast<uint32_t>(std::strtoul(rest + 1, nullptr, 0)) : 0;
    if (length == 0) {
        std::fprintf(stderr, "--dump-mem wants <addr>,<len>\n");
        return false;
    }
    dumps->emplace_back(start, length);
    return true;
}

template <typename Machine>
void printMemDumps(Machine& machine, const MemDumps& dumps) {
    for (const auto& [start, length] : dumps) {
        std::printf("--- memory $%04X+%u ---\n", start, length);
        for (uint32_t a = start; a < start + length && a <= 0xFFFF; a += 16) {
            std::printf("%04X:", a);
            for (uint32_t i = a; i < a + 16 && i < start + length && i <= 0xFFFF; ++i)
                std::printf(" %02X", machine.debugPeek(static_cast<uint16_t>(i)));
            std::printf("\n");
        }
    }
}

}  // namespace cli
