#pragma once
#include <algorithm>
#include <string>
#include <vector>

#include "MemoryCardDefinition.hpp"
#include "NamedFileCatalog.hpp"

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
    return scanNamedFiles<MemoryCardCatalogEntry>(
        dir, ".card.yaml", "module",
        [](const std::string& text, const std::string& path, MemoryCardCatalogEntry* out, std::string* err) {
            MemoryCardDefinition def;
            if (!parseMemoryCardDefinition(text, &def, err)) return false;
            *out = {def.moduleName, def.compatibleHosts, path, def.battery};
            return true;
        },
        [](const MemoryCardCatalogEntry& e) { return e.moduleName; }, error);
}

// Ordered-search resolve of a `modulespec: <module-name>` reference to the
// `.card.yaml` path that declares it: the FIRST directory in `dirs` holding
// exactly one file declaring it wins. The GUI passes [bundled resources,
// save folder], so a preset can name a bundled card or a user's saved
// battery-card instance. A missing / unreadable directory (e.g. the save
// folder before anything was saved) is skipped silently; only two files in
// the SAME directory claiming one name is a hard error. Empty directory
// strings are ignored.
inline bool resolveModuleSpecByName(const std::vector<std::string>& dirs, const std::string& moduleName,
                                    std::string* outPath, std::string* error) {
    return resolveNamedFile(dirs, moduleName, "module", scanMemoryCardDirectory,
                            [](const MemoryCardCatalogEntry& e) { return e.moduleName; }, outPath, error);
}

// Single-directory form of the above.
inline bool resolveModuleSpecByName(const std::string& dir, const std::string& moduleName,
                                    std::string* outPath, std::string* error) {
    return resolveModuleSpecByName(std::vector<std::string>{dir}, moduleName, outPath, error);
}
