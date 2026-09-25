#pragma once
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

// ── Battery-card named-instance writer ─────────────────────────────────
//
// Generates a standalone `<name>.card.yaml` battery-card instance by
// splicing a freshly-dumped `initial-content:` block into the *original
// template's (or, on a re-save, the previous instance's) source text* --
// never by re-serializing a parsed MemoryCardDefinition -- so every
// hand-written doc-comment, flash-protocol block, and addressing/banking
// section survives untouched.
//
// Known v1 limit: assumes exactly one region per card (true of every
// battery card today -- CE-163F, CE-1600M, CE-1601M, CE-1638, superram). A
// future multi-region battery card would need spliceBatteryCardInstance()
// to target a specific region by name.

namespace mcd_detail {
// Forward decl only -- callers that need round-trip verification include
// MemoryCardDefinition.hpp themselves; this header doesn't depend on it.
}

inline std::string currentIso8601Timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmVal{};
#if defined(_WIN32)
    gmtime_s(&tmVal, &t);
#else
    gmtime_r(&t, &tmVal);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tmVal.tm_year + 1900,
                  tmVal.tm_mon + 1, tmVal.tm_mday, tmVal.tm_hour, tmVal.tm_min, tmVal.tm_sec);
    return std::string(buf);
}

// Encodes `bytes` (a whole bank) using the `addressed-hex` dialect
// (docs/Memory-Card-Definition-Format.md §6): 16-byte rows, or
// "$XXXX: XX..." for a run of consecutive whole rows that all share one
// uniform byte value -- exact, not a guess, since the run's end is always
// the next line's address (or the end of the bank, for the last line).
// Never collapses a partial trailing row, so the output stays parseable
// by MemoryCardDefinition.hpp's parseAddressedHex (which requires lines
// to partition the block with no gap/overlap).
inline std::vector<std::string> formatAddressedHexLines(const std::vector<uint8_t>& bytes) {
    std::vector<std::string> lines;
    const size_t n = bytes.size();
    size_t i = 0;
    while (i < n) {
        const size_t rowEnd = std::min(i + 16, n);
        const uint8_t first = bytes[i];
        bool uniform = true;
        for (size_t k = i; k < rowEnd; ++k) {
            if (bytes[k] != first) { uniform = false; break; }
        }
        if (!uniform) {
            // Hand-rolled hex (not snprintf per byte): a whole floppy side is
            // 4096 rows, rewritten on every autosave.
            static const char kHex[] = "0123456789ABCDEF";
            char line[16];
            std::snprintf(line, sizeof(line), "$%04X: ", static_cast<unsigned>(i));
            std::string text(line);
            text.reserve(text.size() + 3 * 16 + 1);
            for (size_t k = i; k < rowEnd; ++k) {
                if (k != i) text += (k - i == 8) ? "  " : " ";
                text += kHex[bytes[k] >> 4];
                text += kHex[bytes[k] & 0x0F];
            }
            lines.push_back(std::move(text));
            i = rowEnd;
            continue;
        }
        size_t runEnd = rowEnd;
        while (runEnd + 16 <= n) {
            bool nextUniform = true;
            for (size_t k = runEnd; k < runEnd + 16; ++k) {
                if (bytes[k] != first) { nextUniform = false; break; }
            }
            if (!nextUniform) break;
            runEnd += 16;
        }
        char line[24];
        std::snprintf(line, sizeof(line), "$%04X: %02X...", static_cast<unsigned>(i), first);
        lines.push_back(line);
        i = runEnd;
    }
    return lines;
}

// Emits the `initial-content:`/`blocks:` lines (4-space region-field
// indent, per docs/Memory-Card-Definition-Format.md), one `- bank:`/`offset:`/
// `encoding: addressed-hex`/`bytes: |` entry per bank. Every bank is
// written, including uniform ones: an omitted bank would reload at the
// region's power-up-fill rather than the value it held (e.g. an erased
// CE-163F flash bank, all 0xFF, would come back as its 0xAA fill), and
// formatAddressedHexLines() already collapses a uniform bank to a single
// "$0000: XX..." line, so there is nothing to save by dropping it.
//
// `bankCountOrNegativeForUnbanked` follows ExpansionCard::debugBankCount()'s
// own convention: a positive count for a genuinely banked region (its
// initial-content blocks each need a `bank:` key -- even when that count
// is 1), or <= 0 for a region with no bank concept at all (a single
// implicit "bank" whose blocks must NOT carry a `bank:` key -- Core's
// parseInitialContent() rejects `bank:` on an unbanked region). Passing
// through the raw signed value from debugBankCount() (rather than
// collapsing it to 1 beforehand) is required for this to come out right.
inline std::vector<std::string> formatBatteryCardInitialContentBlock(int bankCountOrNegativeForUnbanked,
                                                                      const std::vector<uint8_t>& image) {
    const bool banked = bankCountOrNegativeForUnbanked > 0;
    const int bankCount = banked ? bankCountOrNegativeForUnbanked : 1;

    std::vector<std::string> lines = {"    initial-content:", "      blocks:"};
    const size_t bankSize = image.size() / static_cast<size_t>(bankCount);
    for (int bank = 0; bank < bankCount; ++bank) {
        const auto begin = image.begin() + static_cast<long>(static_cast<size_t>(bank) * bankSize);
        const std::vector<uint8_t> bytes(begin, begin + static_cast<long>(bankSize));
        if (banked) {
            char bankLine[32];
            std::snprintf(bankLine, sizeof(bankLine), "        - bank: %d", bank);
            lines.push_back(bankLine);
            lines.push_back("          offset: 0x0000");
        } else {
            lines.push_back("        - offset: 0x0000");
        }
        lines.push_back("          encoding: addressed-hex");
        lines.push_back("          bytes: |");
        for (const auto& hexLine : formatAddressedHexLines(bytes)) lines.push_back("            " + hexLine);
    }
    return lines;
}

namespace bci_detail {

inline int lineIndent(const std::string& line) {
    int n = 0;
    while (n < static_cast<int>(line.size()) && line[static_cast<size_t>(n)] == ' ') ++n;
    return n;
}

inline std::string trimmed(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

inline bool isBlank(const std::string& line) { return trimmed(line).empty(); }

inline bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

inline bool isRegionItem(const std::string& line) {
    return lineIndent(line) == 2 && startsWith(trimmed(line), "- name:");
}

// Replaces (or, if absent, inserts) the sole region's `initial-content:`
// block in place. Returns false if no `regions:` list with a region item
// was found at all.
inline bool spliceRegionInitialContent(std::vector<std::string>* lines,
                                       const std::vector<std::string>& contentLines) {
    int regionsIdx = -1;
    for (size_t i = 0; i < lines->size(); ++i) {
        if (startsWith((*lines)[i], "regions:")) { regionsIdx = static_cast<int>(i); break; }
    }
    if (regionsIdx < 0) return false;

    int regionStart = -1;
    for (size_t i = static_cast<size_t>(regionsIdx) + 1; i < lines->size(); ++i) {
        if (isRegionItem((*lines)[i])) { regionStart = static_cast<int>(i); break; }
    }
    if (regionStart < 0) return false;

    int regionEnd = static_cast<int>(lines->size());
    for (size_t i = static_cast<size_t>(regionStart) + 1; i < lines->size(); ++i) {
        if (isBlank((*lines)[i])) continue;
        if (lineIndent((*lines)[i]) <= 2) { regionEnd = static_cast<int>(i); break; }
    }

    int existingContentStart = -1;
    for (int i = regionStart; i < regionEnd; ++i) {
        if (lineIndent((*lines)[static_cast<size_t>(i)]) == 4 &&
            startsWith(trimmed((*lines)[static_cast<size_t>(i)]), "initial-content:")) {
            existingContentStart = i;
            break;
        }
    }

    if (existingContentStart >= 0) {
        int end = regionEnd;
        for (int i = existingContentStart + 1; i < regionEnd; ++i) {
            if (isBlank((*lines)[static_cast<size_t>(i)])) continue;
            if (lineIndent((*lines)[static_cast<size_t>(i)]) <= 4) { end = i; break; }
        }
        lines->erase(lines->begin() + existingContentStart, lines->begin() + end);
        lines->insert(lines->begin() + existingContentStart, contentLines.begin(), contentLines.end());
    } else {
        int insertAt = regionEnd;
        while (insertAt > regionStart && isBlank((*lines)[static_cast<size_t>(insertAt - 1)])) --insertAt;
        lines->insert(lines->begin() + insertAt, contentLines.begin(), contentLines.end());
    }
    return true;
}

}  // namespace bci_detail

// Splices `contentLines` (an `initial-content:` block, e.g. from
// formatBatteryCardInitialContentBlock) into `sourceText` under the file's
// one region, renames the card to
// `newModuleName`, and (re)writes a generated-instance header comment.
// `sourceText` is either a template (first save) or an instance file's
// own text (a re-save/autosave, or a save-as of an instance) -- either
// way the existing `created:` timestamp and "generated ... from" origin,
// if any, are preserved (`sourceModuleName` only names the origin of a
// file that has none yet). A top-level `template:` line is dropped: the
// result is always an instance. Returns
// false (and fills *error) if `sourceText` doesn't contain a
// `module-name:` line or a `regions:` list with at least one region --
// i.e. isn't a valid card definition to begin with.
inline bool spliceBatteryCardInstance(const std::string& sourceText, const std::string& newModuleName,
                                      const std::string& sourceModuleName,
                                      const std::vector<std::string>& contentLines, std::string* outText,
                                      std::string* error,
                                      const std::string& nowIso8601 = currentIso8601Timestamp()) {
    using namespace bci_detail;

    std::vector<std::string> lines;
    {
        size_t pos = 0;
        while (true) {
            size_t nl = sourceText.find('\n', pos);
            if (nl == std::string::npos) {
                lines.push_back(sourceText.substr(pos));
                break;
            }
            lines.push_back(sourceText.substr(pos, nl - pos));
            pos = nl + 1;
        }
    }

    static const std::string kHeaderPrefix = "# Battery-card instance";
    static const std::string kCreatedPrefix = "# created: ";

    std::string createdAt;
    bool hadCreatedAt = false;
    std::string originLine = kHeaderPrefix + " -- generated by Calc-U-1600 from \"" + sourceModuleName + "\".";
    if (!lines.empty() && startsWith(lines.front(), kHeaderPrefix)) {
        originLine = lines.front();
        size_t i = 0;
        while (i < lines.size() && startsWith(lines[i], "#")) {
            if (startsWith(lines[i], kCreatedPrefix)) {
                createdAt = lines[i].substr(kCreatedPrefix.size());
                hadCreatedAt = true;
            }
            ++i;
        }
        while (i < lines.size() && lines[i].empty()) ++i;
        lines.erase(lines.begin(), lines.begin() + static_cast<long>(i));
    }

    int moduleNameIdx = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (startsWith(lines[i], "module-name:")) { moduleNameIdx = static_cast<int>(i); break; }
    }
    if (moduleNameIdx < 0) {
        if (error) *error = "source text has no 'module-name:' line";
        return false;
    }
    lines[static_cast<size_t>(moduleNameIdx)] = "module-name: \"" + newModuleName + "\"";
    lines.erase(std::remove_if(lines.begin(), lines.end(),
                               [](const std::string& l) { return startsWith(l, "template:"); }),
                lines.end());

    if (!spliceRegionInitialContent(&lines, contentLines)) {
        if (error) *error = "source text has no 'regions:' list with a region item";
        return false;
    }

    std::vector<std::string> header = {
        originLine,
        kCreatedPrefix + (hadCreatedAt ? createdAt : nowIso8601),
        "# last-saved: " + nowIso8601,
        "",
    };

    std::string result;
    for (const auto& l : header) { result += l; result += '\n'; }
    for (size_t i = 0; i < lines.size(); ++i) {
        result += lines[i];
        if (i + 1 < lines.size()) result += '\n';
    }
    if (outText) *outText = result;
    return true;
}
