#pragma once
#include <filesystem>
#include <string>

#include "../Debug/AttachKeys.hpp"
#include "../Yaml.hpp"
#include "PresetFile.hpp"

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
// values against the attach-key table (Core/Debug/AttachKeys.hpp) and
// resolves every file path against the preset's directory, in place, so the
// node can be handed on as it is.
namespace preset_debug {

using namespace debug::attach;

inline bool resolvePathScalar(YamlNode* node, const std::filesystem::path& dir, std::string* error) {
    std::string value;
    if (!node->isScalar()) {
        *error = yaml_detail::errAt(node->line, "expected a path");
        return false;
    }
    if (!node->asString(&value, error)) return false;
    node->scalar = resolvePath(dir, value);
    return true;
}

inline bool checkMap(YamlNode* map, const Table& table, const std::filesystem::path& dir, std::string* error);

// One value in the shape its key takes; paths resolved against `dir`.
inline bool checkValue(const std::string& name, const Key& key, YamlNode* v, const std::filesystem::path& dir,
                       std::string* error) {
    switch (key.kind) {
        case Kind::Map:
            if (!v->isMap()) {
                *error = yaml_detail::errAt(v->line, "'" + name + ":' takes a block of settings");
                return false;
            }
            if (!checkMap(v, kProgramKeys, dir, error)) return false;
            if (!v->has("bin")) {
                *error = yaml_detail::errAt(v->line, "'" + name + ":' needs at least 'bin'");
                return false;
            }
            return true;
        case Kind::FileList:
            if (!v->isSeq()) {
                *error = yaml_detail::errAt(v->line, "'" + name + ":' expects a list");
                return false;
            }
            for (YamlNode& item : v->seq) {
                if (item.isScalar()) {
                    if (!resolvePathScalar(&item, dir, error)) return false;
                } else if (!item.isMap() || !item.has("path")) {
                    *error = yaml_detail::errAt(item.line, "expected a path, or a map with 'path'");
                    return false;
                } else if (!checkMap(&item, itemKeys(key), dir, error)) {
                    return false;
                }
            }
            return true;
        case Kind::PathList:
            if (!v->isSeq()) {
                *error = yaml_detail::errAt(v->line, "'" + name + ":' expects a list of paths");
                return false;
            }
            for (YamlNode& item : v->seq)
                if (!resolvePathScalar(&item, dir, error)) return false;
            return true;
        case Kind::Path: return resolvePathScalar(v, dir, error);
        default: break;
    }
    std::string text, why;
    if (!v->isScalar()) {
        *error = yaml_detail::errAt(v->line, "'" + name + "' takes a single value");
        return false;
    }
    if (!v->asString(&text, error)) return false;
    // A quoted `"true"` would reach the debugger as text, not a boolean.
    const bool quotedBool = key.kind == Kind::Bool && text != v->scalar;
    if (quotedBool || !checkScalar(key, text, &why)) {
        if (quotedBool) why = "true or false, without quotes";
        *error = yaml_detail::errAt(v->line, "'" + name + "' is " + why + ", not '" + text + "'");
        return false;
    }
    return true;
}

// Only the keys of `table`, each in its shape.
inline bool checkMap(YamlNode* map, const Table& table, const std::filesystem::path& dir, std::string* error) {
    for (auto& kv : map->map) {
        const Key* key = find(table, kv.first);
        if (!key) {
            *error = yaml_detail::errAt(map->line, "unknown key '" + kv.first + "'");
            return false;
        }
        if (!checkValue(kv.first, *key, &kv.second, dir, error)) return false;
    }
    return true;
}

} // namespace preset_debug

/// Checks a parsed `debug:` block against the attach keys and resolves its
/// paths against `presetDir`.
inline bool parsePresetDebugBlock(YamlNode* block, const std::filesystem::path& presetDir, std::string* error) {
    if (!block->isMap()) {
        *error = yaml_detail::errAt(block->line, "'debug:' takes a block of settings");
        return false;
    }
    return preset_debug::checkMap(block, debug::attach::kBlockKeys, presetDir, error);
}
