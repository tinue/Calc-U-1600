#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "../Yaml.hpp"
#include "BatteryCardInstance.hpp"   // formatAddressedHexLines, currentIso8601Timestamp
#include "CE1600FCard.hpp"
#include "MemoryCardDefinition.hpp"  // mcd_detail::parseAddressedHex

// ── CE-1600F floppy-disk file (`<name>.floppy.yaml`) ───────────────────
//
// A saved CE-1600F diskette: an explicit `disk-name` (what the GUI picker
// shows and a preset's `floppy:` key refers to -- the counterpart of a
// card's `module-name`) plus both 64 KB sides as `addressed-hex` blocks
// (docs/Memory-Card-Definition-Format.md §6), so an unused side or track
// run collapses to one line:
//
//   format: ce1600f-floppy
//   format-version: 1
//   disk-name: "Progs"
//   saved: 2026-09-19T12:34:56Z
//   sides:
//     a:
//       encoding: addressed-hex
//       bytes: |
//         $0000: ...
//     b:
//       ...
//
// `format-version` is mandatory; a reader rejects any version it doesn't
// know rather than guessing. The file is always written whole (no splice /
// comment preservation, unlike BatteryCardInstance.hpp's card instances).
//
// Directory lookup mirrors MemoryCardCatalog.hpp: the caller passes the
// search directories (bundled first, then the user's save folder), and a
// name resolves to the first directory holding exactly one file that
// declares it.

constexpr const char* kFloppyFileSuffix = ".floppy.yaml";
constexpr long kFloppyFormatVersion = 1;

struct FloppyFile {
    std::string diskName;
    std::vector<uint8_t> image;  // CE1600FCard::kImageSize bytes, side A then side B
};

namespace floppy_detail {

constexpr const char* kFormatTag = "ce1600f-floppy";
constexpr const char* kSideKeys[2] = {"a", "b"};

inline bool hasFloppySuffix(const std::string& fileName) {
    const std::string suffix = kFloppyFileSuffix;
    return fileName.size() > suffix.size() &&
           fileName.compare(fileName.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool readTextFile(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

// Checks `format`/`format-version` and reads `disk-name` from a parsed file.
inline bool readHeader(const YamlNode& root, std::string* diskName, std::string* error) {
    if (!root.isMap()) {
        *error = "not a floppy-disk file (expected a YAML mapping)";
        return false;
    }
    if (!root.requireOnlyKeys({"format", "format-version", "disk-name", "saved", "sides"}, error)) return false;
    std::string format;
    const YamlNode* formatNode = root.find("format");
    if (!formatNode || !formatNode->asString(&format, error) || format != kFormatTag) {
        *error = std::string("not a floppy-disk file (expected 'format: ") + kFormatTag + "')";
        return false;
    }
    const YamlNode* versionNode = root.find("format-version");
    long version = 0;
    if (!versionNode) {
        *error = "missing 'format-version'";
        return false;
    }
    if (!versionNode->asInt(&version, error)) return false;
    if (version != kFloppyFormatVersion) {
        *error = "unsupported floppy format-version " + std::to_string(version) + " (this build reads " +
                 std::to_string(kFloppyFormatVersion) + ")";
        return false;
    }
    const YamlNode* nameNode = root.find("disk-name");
    if (!nameNode) {
        *error = "missing 'disk-name'";
        return false;
    }
    if (!nameNode->asString(diskName, error)) return false;
    if (diskName->empty()) {
        *error = "empty 'disk-name'";
        return false;
    }
    return true;
}

}  // namespace floppy_detail

// Serializes `image` (must be CE1600FCard::kImageSize bytes) as a complete
// version-1 file. `diskName` is written double-quoted verbatim (the YAML
// reader does no escape processing), so it must not contain '"' or a newline.
inline std::string formatFloppyFile(const std::string& diskName, const std::vector<uint8_t>& image) {
    std::string out;
    out += "# Calc-U-1600 CE-1600F floppy disk image\n";
    out += std::string("format: ") + floppy_detail::kFormatTag + "\n";
    out += "format-version: " + std::to_string(kFloppyFormatVersion) + "\n";
    out += "disk-name: \"" + diskName + "\"\n";
    out += "saved: " + currentIso8601Timestamp() + "\n";
    out += "sides:\n";
    for (size_t side = 0; side < 2; ++side) {
        const auto first = image.begin() + static_cast<std::ptrdiff_t>(side * CE1600FCard::kSideSize);
        const std::vector<uint8_t> bytes(first, first + static_cast<std::ptrdiff_t>(CE1600FCard::kSideSize));
        out += std::string("  ") + floppy_detail::kSideKeys[side] + ":\n";
        out += "    encoding: addressed-hex\n";
        out += "    bytes: |\n";
        for (const auto& line : formatAddressedHexLines(bytes)) out += "      " + line + "\n";
    }
    return out;
}

inline bool parseFloppyFile(const std::string& text, FloppyFile* out, std::string* error) {
    YamlNode root;
    if (!parseYaml(text, &root, error)) return false;
    if (!floppy_detail::readHeader(root, &out->diskName, error)) return false;
    const YamlNode* sides = root.find("sides");
    if (!sides || !sides->isMap()) {
        *error = "missing 'sides' mapping";
        return false;
    }
    if (!sides->requireOnlyKeys({"a", "b"}, error)) return false;
    out->image.clear();
    out->image.reserve(CE1600FCard::kImageSize);
    for (const char* key : floppy_detail::kSideKeys) {
        const std::string where = std::string("side '") + key + "': ";
        const YamlNode* side = sides->find(key);
        if (!side || !side->isMap()) {
            *error = where + "missing";
            return false;
        }
        if (!side->requireOnlyKeys({"encoding", "bytes"}, error)) return false;
        std::string encoding, bytesText;
        const YamlNode* encNode = side->find("encoding");
        const YamlNode* bytesNode = side->find("bytes");
        if (!encNode || !encNode->asString(&encoding, error) || encoding != "addressed-hex") {
            *error = where + "expected 'encoding: addressed-hex'";
            return false;
        }
        if (!bytesNode || !bytesNode->asString(&bytesText, error)) {
            *error = where + "missing 'bytes'";
            return false;
        }
        std::vector<uint8_t> bytes;
        if (!mcd_detail::parseAddressedHex(bytesText, static_cast<uint32_t>(CE1600FCard::kSideSize), &bytes,
                                           error)) {
            *error = where + *error;
            return false;
        }
        out->image.insert(out->image.end(), bytes.begin(), bytes.end());
    }
    return true;
}

inline bool readFloppyFile(const std::string& path, FloppyFile* out, std::string* error) {
    std::string text;
    if (!floppy_detail::readTextFile(path, &text)) {
        *error = "cannot read '" + path + "'";
        return false;
    }
    if (!parseFloppyFile(text, out, error)) {
        *error = path + ": " + *error;
        return false;
    }
    return true;
}

struct FloppyCatalogEntry {
    std::string diskName;
    std::string filePath;
};

// Every "*.floppy.yaml" in `dir` (non-recursive), sorted by disk name. Only
// the header is validated (the hex isn't decoded). A bad file is skipped
// with "<filename>: <error>" appended to `*error`; a missing directory
// yields an empty list and sets `*error` -- same contract as
// scanMemoryCardDirectory().
inline std::vector<FloppyCatalogEntry> scanFloppyDirectory(const std::string& dir, std::string* error) {
    std::vector<FloppyCatalogEntry> out;
    auto appendErr = [&](const std::string& msg) {
        if (!error) return;
        if (!error->empty()) *error += "\n";
        *error += msg;
    };

    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    const std::filesystem::directory_iterator end;
    if (ec) {
        if (error) *error = "cannot read floppy directory '" + dir + "': " + ec.message();
        return out;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            appendErr("directory walk stopped: " + ec.message());
            break;
        }
        const std::filesystem::path& p = it->path();
        const std::string name = p.filename().string();
        if (!floppy_detail::hasFloppySuffix(name)) continue;

        std::string text, parseErr, diskName;
        YamlNode root;
        if (!floppy_detail::readTextFile(p.string(), &text)) {
            appendErr(name + ": cannot open");
            continue;
        }
        if (!parseYaml(text, &root, &parseErr) || !floppy_detail::readHeader(root, &diskName, &parseErr)) {
            appendErr(name + ": " + parseErr);
            continue;
        }
        out.push_back({diskName, p.string()});
    }
    std::sort(out.begin(), out.end(),
              [](const FloppyCatalogEntry& a, const FloppyCatalogEntry& b) { return a.diskName < b.diskName; });
    return out;
}

// Resolves `diskName` against `dirs` in order: the first directory holding
// exactly one file that declares it wins; two in the same directory is an
// error. Missing directories and empty strings are skipped. Same semantics
// as resolveModuleSpecByName(dirs, ...).
inline bool resolveFloppyByName(const std::vector<std::string>& dirs, const std::string& diskName,
                                std::string* outPath, std::string* error) {
    std::string where;
    for (const auto& dir : dirs) {
        if (dir.empty()) continue;
        if (!where.empty()) where += ", ";
        where += "'" + dir + "'";
        const auto entries = scanFloppyDirectory(dir, nullptr);
        const FloppyCatalogEntry* hit = nullptr;
        for (const auto& e : entries) {
            if (e.diskName != diskName) continue;
            if (hit) {
                if (error)
                    *error = "disk name '" + diskName + "' is declared by more than one file in '" + dir +
                             "' (" + hit->filePath + ", " + e.filePath + ")";
                return false;
            }
            hit = &e;
        }
        if (hit) {
            if (outPath) *outPath = hit->filePath;
            return true;
        }
    }
    if (error)
        *error = "no floppy disk named '" + diskName + "' in " + (where.empty() ? "any directory" : where);
    return false;
}
