#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../Yaml.hpp"
#include "BatteryCardInstance.hpp"   // formatAddressedHexLines, currentIso8601Timestamp
#include "CE1600FCard.hpp"
#include "MemoryCardDefinition.hpp"  // mcd_detail::parseAddressedHex
#include "NamedFileCatalog.hpp"

// ── CE-1600F floppy-disk file (`<name>.floppy.yaml`) ───────────────────
//
// Specification: docs/Floppy-Image-Format.md (other tools, e.g.
// SharpDataExchange, implement that document -- keep the two in step).
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
// Directory lookup is NamedFileCatalog.hpp's, shared with memory cards: the
// caller passes the search directories (bundled first, then the user's save
// folder), and a name resolves to the first directory holding exactly one
// file that declares it.

constexpr const char* kFloppyFileSuffix = ".floppy.yaml";
constexpr long kFloppyFormatVersion = 1;

struct FloppyFile {
    std::string diskName;
    std::vector<uint8_t> image;  // CE1600FCard::kImageSize bytes, side A then side B
};

namespace floppy_detail {

constexpr const char* kFormatTag = "ce1600f-floppy";
constexpr const char* kSideKeys[2] = {"a", "b"};

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
    out.reserve(4096);
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
        for (const auto& line : formatAddressedHexLines(bytes)) {
            out += "      ";
            out += line;
            out += '\n';
        }
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
    if (!named_file_detail::readTextFile(path, &text)) {
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

// Every "*.floppy.yaml" in `dir` (non-recursive), sorted by disk name --
// scanNamedFiles()'s contract. Only the header (the text before the
// top-level `sides:` key) is parsed, so a large disk costs no hex decoding.
inline std::vector<FloppyCatalogEntry> scanFloppyDirectory(const std::string& dir, std::string* error) {
    return scanNamedFiles<FloppyCatalogEntry>(
        dir, kFloppyFileSuffix, "disk",
        [](const std::string& text, const std::string& path, FloppyCatalogEntry* out, std::string* err) {
            const size_t sides = text.find("\nsides:");
            YamlNode root;
            if (!parseYaml(text.substr(0, sides), &root, err) ||
                !floppy_detail::readHeader(root, &out->diskName, err))
                return false;
            out->filePath = path;
            return true;
        },
        [](const FloppyCatalogEntry& e) { return e.diskName; }, error);
}

// Resolves `diskName` against `dirs` in order (bundled first, then the save
// folder) -- resolveNamedFile()'s rules, the same as memory cards.
inline bool resolveFloppyByName(const std::vector<std::string>& dirs, const std::string& diskName,
                                std::string* outPath, std::string* error) {
    return resolveNamedFile(dirs, diskName, "disk", scanFloppyDirectory,
                            [](const FloppyCatalogEntry& e) { return e.diskName; }, outPath, error);
}
