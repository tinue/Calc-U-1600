#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "MemoryCardDefinition.hpp"

// ── Catalogue of on-disk memory-card definition files ─────────────────
//
// Scans a directory of docs/Memory-Card-Definition-Format.md `.card.yaml`
// files and indexes them by `module-name`. Two consumers:
//   * the GUI lists every module whose `compatible-hosts` covers the open
//     model / slot (see the control-bar module picker);
//   * a preset's `modulespec: <module-name>` (as opposed to the by-path
//     `modulespecfile: <path>`) resolves the name to a file here, then
//     hands the path to makeSoftwareDefinedCard() unchanged.
//
// No notion of "app resource" reaches the core: the caller supplies the
// directory -- the GUI its bundled resource path, the CLIs their
// `--modules-dir` (default the repo's Calc-U-1600/Resources) -- exactly as
// it already supplies romPath / traceDir to the preset loaders.

struct MemoryCardCatalogEntry {
    std::string moduleName;
    std::vector<CardHost> compatibleHosts;
    std::string filePath;
    bool battery = false;

    bool compatibleWith(CardHost h) const {
        return std::find(compatibleHosts.begin(), compatibleHosts.end(), h) != compatibleHosts.end();
    }
};

// Parse every "*.card.yaml" in `dir` (non-recursive). A file that fails to
// parse is skipped, with "<filename>: <error>" appended to `*error`
// (newline-separated) -- one malformed file never hides the rest. A
// missing / unreadable directory yields an empty list and sets `*error`.
// Entries come back sorted by `moduleName` for a stable GUI order.
inline std::vector<MemoryCardCatalogEntry> scanMemoryCardDirectory(const std::string& dir,
                                                                   std::string* error) {
    std::vector<MemoryCardCatalogEntry> out;
    auto appendErr = [&](const std::string& msg) {
        if (!error) return;
        if (!error->empty()) *error += "\n";
        *error += msg;
    };

    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    const std::filesystem::directory_iterator end;
    if (ec) {
        if (error) *error = "cannot read module directory '" + dir + "': " + ec.message();
        return out;
    }

    for (; it != end; it.increment(ec)) {
        if (ec) {
            appendErr("directory walk stopped: " + ec.message());
            break;
        }
        const std::filesystem::path& p = it->path();
        // ".card.yaml" is a double extension -- path::extension() only sees
        // ".yaml" -- so match the filename tail directly.
        const std::string name = p.filename().string();
        static const std::string kSuffix = ".card.yaml";
        if (name.size() <= kSuffix.size() ||
            name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
            continue;

        std::ifstream in(p, std::ios::binary);
        if (!in) {
            appendErr(name + ": cannot open");
            continue;
        }
        std::stringstream ss;
        ss << in.rdbuf();

        MemoryCardDefinition def;
        std::string parseErr;
        if (!parseMemoryCardDefinition(ss.str(), &def, &parseErr)) {
            appendErr(name + ": " + parseErr);
            continue;
        }
        out.push_back({def.moduleName, def.compatibleHosts, p.string(), def.battery});
    }

    std::sort(out.begin(), out.end(),
              [](const MemoryCardCatalogEntry& a, const MemoryCardCatalogEntry& b) {
                  return a.moduleName < b.moduleName;
              });
    return out;
}

// Resolve a `modulespec: <module-name>` reference to the `.card.yaml` path
// that declares it, scanning `dir`. Returns false and fills `*error` when
// the directory is unreadable, no file declares `moduleName`, or more than
// one does (an ambiguous catalogue is a setup error, not a pick-one case).
inline bool resolveModuleSpecByName(const std::string& dir, const std::string& moduleName,
                                    std::string* outPath, std::string* error) {
    std::string scanErr;
    const auto entries = scanMemoryCardDirectory(dir, &scanErr);

    const MemoryCardCatalogEntry* hit = nullptr;
    for (const auto& e : entries) {
        if (e.moduleName != moduleName) continue;
        if (hit) {
            if (error)
                *error = "module name '" + moduleName + "' is declared by more than one file in '" +
                         dir + "' (" + hit->filePath + ", " + e.filePath + ")";
            return false;
        }
        hit = &e;
    }
    if (!hit) {
        if (error) {
            *error = "no module named '" + moduleName + "' in '" + dir + "'";
            if (!scanErr.empty()) *error += " (some files failed to parse: " + scanErr + ")";
        }
        return false;
    }
    if (outPath) *outPath = hit->filePath;
    return true;
}

// Ordered-search variant: resolve `moduleName` against `dirs` in order and
// return the path from the FIRST directory that contains exactly one file
// declaring it. Used so a preset's `modulespec: <module-name>` can name a
// bundled/standard card OR a user's named battery-card instance saved to
// iCloud Drive -- the GUI passes [bundled resources, iCloud BatteryCards].
// A missing / unreadable directory (the iCloud folder before any instance
// has been saved, or while offline) is skipped silently; only two files in
// the SAME directory claiming one name is a hard error (an ambiguous
// catalogue is a setup mistake, not a pick-one case). Empty directory
// strings are ignored. With a single directory this matches the overload
// above exactly.
inline bool resolveModuleSpecByName(const std::vector<std::string>& dirs,
                                    const std::string& moduleName,
                                    std::string* outPath, std::string* error) {
    std::vector<std::string> searched;
    std::string scanNotes;
    for (const auto& dir : dirs) {
        if (dir.empty()) continue;
        searched.push_back(dir);

        std::string scanErr;
        const auto entries = scanMemoryCardDirectory(dir, &scanErr);

        const MemoryCardCatalogEntry* hit = nullptr;
        for (const auto& e : entries) {
            if (e.moduleName != moduleName) continue;
            if (hit) {
                if (error)
                    *error = "module name '" + moduleName +
                             "' is declared by more than one file in '" + dir + "' (" +
                             hit->filePath + ", " + e.filePath + ")";
                return false;
            }
            hit = &e;
        }
        if (hit) {
            if (outPath) *outPath = hit->filePath;
            return true;
        }
        if (!scanErr.empty()) {
            if (!scanNotes.empty()) scanNotes += "; ";
            scanNotes += scanErr;
        }
    }
    if (error) {
        std::string where;
        for (const auto& d : searched) {
            if (!where.empty()) where += ", ";
            where += "'" + d + "'";
        }
        *error = "no module named '" + moduleName + "' in " +
                 (where.empty() ? "any module directory" : where);
        if (!scanNotes.empty()) *error += " (some files failed to parse: " + scanNotes + ")";
    }
    return false;
}
