#pragma once
#include <filesystem>
#include <string>

#include "../Yaml.hpp"

// ── A preset's `debug:` block ────────────────────────────────────────────
//
// A project preset (docs/Debugger.md) carries the debugger's attach
// settings next to the machine it describes, so every IDE finds them in one
// file. The block has the attach configuration's own keys and shapes:
//
//   debug:
//     stopOnEntry: true
//     command: CALL &C0C5,1         # typed after the clean start and load
//     program:
//       bin: hello.bin
//       listing: hello.lst
//       entry: START
//     listings:
//       - path: rom/bank7.lst
//         cpu: z80
//         bank: 7
//     symbols:
//       - rom/pc1600.sym
//
// The preset runner ignores the block; only the debugger reads it
// (Qt6/app/debug/DapSession). parsePresetDebugBlock() checks the keys and
// resolves every file path against the preset's directory, in place, so the
// node can be handed on as it is.
namespace preset_debug {

inline bool resolvePathScalar(YamlNode* node, const std::filesystem::path& dir, std::string* error) {
    std::string value;
    if (!node->asString(&value, error)) return false;
    const std::filesystem::path p(value);
    node->scalar = p.is_absolute() ? value : (dir / p).lexically_normal().string();
    return true;
}

inline bool resolvePathKey(YamlNode* map, const char* key, const std::filesystem::path& dir, std::string* error) {
    for (auto& kv : map->map)
        if (kv.first == key) return resolvePathScalar(&kv.second, dir, error);
    return true;
}

// `listings:` / `symbols:` items are a path, or a map with `path`, `cpu`
// and bank qualifiers (`source` too, for a listing).
inline bool resolveFileList(YamlNode* list, bool listings, const std::filesystem::path& dir, std::string* error) {
    if (!list->isSeq()) {
        *error = yaml_detail::errAt(list->line, "expected a list");
        return false;
    }
    for (YamlNode& item : list->seq) {
        if (item.isScalar()) {
            if (!resolvePathScalar(&item, dir, error)) return false;
            continue;
        }
        if (!item.isMap() || !item.has("path")) {
            *error = yaml_detail::errAt(item.line, "expected a path, or a map with 'path'");
            return false;
        }
        if (listings ? !item.requireOnlyKeys({"path", "source", "cpu", "bank", "me", "pu", "pv"}, error)
                     : !item.requireOnlyKeys({"path", "cpu", "bank", "me", "pu", "pv"}, error))
            return false;
        if (!resolvePathKey(&item, "path", dir, error) || !resolvePathKey(&item, "source", dir, error)) return false;
    }
    return true;
}

} // namespace preset_debug

/// Checks a parsed `debug:` block and resolves its paths against `presetDir`.
inline bool parsePresetDebugBlock(YamlNode* block, const std::filesystem::path& presetDir, std::string* error) {
    using namespace preset_debug;
    if (!block->isMap()) {
        *error = yaml_detail::errAt(block->line, "'debug:' takes a block of settings");
        return false;
    }
    if (!block->requireOnlyKeys({"reset", "stopOnEntry", "command", "program", "listings", "symbols", "boot"}, error))
        return false;
    for (auto& kv : block->map) {
        YamlNode& v = kv.second;
        if (kv.first == "program") {
            if (!v.isMap() || !v.has("bin")) {
                *error = yaml_detail::errAt(v.line, "'program:' needs at least 'bin'");
                return false;
            }
            if (!v.requireOnlyKeys({"bin", "listing", "source", "symbols", "cpu", "address", "entry", "after",
                                    "cleanStart", "bank", "me", "pu", "pv"},
                                   error))
                return false;
            if (!resolvePathKey(&v, "bin", presetDir, error) || !resolvePathKey(&v, "listing", presetDir, error) ||
                !resolvePathKey(&v, "source", presetDir, error))
                return false;
            for (auto& p : v.map)
                if (p.first == "symbols") {
                    if (!p.second.isSeq()) {
                        *error = yaml_detail::errAt(p.second.line, "'symbols:' expects a list of paths");
                        return false;
                    }
                    for (YamlNode& s : p.second.seq)
                        if (!resolvePathScalar(&s, presetDir, error)) return false;
                }
        } else if (kv.first == "listings" || kv.first == "symbols") {
            if (!resolveFileList(&v, kv.first == "listings", presetDir, error)) return false;
        } else if (!v.isScalar()) {
            *error = yaml_detail::errAt(v.line, "'" + kv.first + "' takes a single value");
            return false;
        }
    }
    return true;
}
