#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../Preset/PresetFile.hpp"

// ── The debugger's attach keys ───────────────────────────────────────────
//
// One table for the keys a launch configuration and a project preset's
// `debug:` block share (docs/Debugger.md). The preset parser
// (Core/Preset/PresetDebugBlock.hpp) takes exactly these keys, checks their
// values and resolves the paths; the debug adapter (Qt6/app/debug/DapSession)
// checks the same values in a launch configuration, which also carries the
// IDE's own keys. `project` and `preset` are launch-configuration keys only.
namespace debug::attach {

enum class Kind {
    Text,     ///< any text
    Path,     ///< a file, relative to the preset
    PathList, ///< a list of files
    Bool,     ///< true / false
    Number,   ///< the preset number rule, 0..max
    Choice,   ///< one of `choices`
    Map,      ///< `program`: the program keys
    FileList, ///< `listings` / `symbols`: items are a path or a map of the listing / symbol keys
};

struct Key {
    const char* name;
    Kind kind;
    uint32_t max = 0;                   ///< Number
    std::vector<const char*> choices{}; ///< Choice
};

using Table = std::vector<Key>;

inline const std::vector<const char*> kCpus{"lh5801", "z80", "lh5803"};

/// The top level of the attach configuration.
inline const Table kBlockKeys{
    {"reset", Kind::Choice, 0, {"none", "reset", "allReset"}},
    {"stopOnEntry", Kind::Bool},
    {"command", Kind::Text},
    {"program", Kind::Map},
    {"listings", Kind::FileList},
    {"symbols", Kind::FileList},
    {"boot", Kind::Choice, 0, {"debug"}},
};

/// `program:` -- the program to load.
inline const Table kProgramKeys{
    {"bin", Kind::Path},
    {"listing", Kind::Path},
    {"source", Kind::Path},
    {"symbols", Kind::PathList},
    {"cpu", Kind::Choice, 0, kCpus},
    {"address", Kind::Number, 0xFFFF},
    {"entry", Kind::Text},
    {"after", Kind::Choice, 0, {"stopOnEntry", "call", "none"}},
    {"cleanStart", Kind::Bool},
    {"bank", Kind::Number, 7},
    {"me", Kind::Number, 1},
    {"pu", Kind::Number, 1},
    {"pv", Kind::Number, 1},
};

/// A `listings:` item given as a map.
inline const Table kListingKeys{
    {"path", Kind::Path},
    {"source", Kind::Path},
    {"cpu", Kind::Choice, 0, kCpus},
    {"bank", Kind::Number, 7},
    {"me", Kind::Number, 1},
    {"pu", Kind::Number, 1},
    {"pv", Kind::Number, 1},
};

/// A `symbols:` item given as a map.
inline const Table kSymbolKeys{
    {"path", Kind::Path},
    {"cpu", Kind::Choice, 0, kCpus},
    {"bank", Kind::Number, 7},
    {"me", Kind::Number, 1},
    {"pu", Kind::Number, 1},
    {"pv", Kind::Number, 1},
};

inline const Key* find(const Table& table, const std::string& name) {
    for (const Key& k : table)
        if (name == k.name) return &k;
    return nullptr;
}

/// The item table of a FileList key.
inline const Table& itemKeys(const Key& key) {
    return std::string(key.name) == "listings" ? kListingKeys : kSymbolKeys;
}

/// Checks the text of a Number, Choice or Bool value (any other kind
/// passes). On failure `why` says what the key takes, e.g. "one of
/// stopOnEntry, call, none".
inline bool checkScalar(const Key& key, const std::string& text, std::string* why) {
    switch (key.kind) {
        case Kind::Number: {
            uint32_t v = 0;
            if (parseNumber(text, &v) && v <= key.max) return true;
            if (key.max > 0xFF) {
                char hex[8];
                std::snprintf(hex, sizeof hex, "&%X", key.max);
                *why = std::string("a number up to ") + hex + " (&, 0x or $ for hex)";
            } else {
                *why = "a number from 0 to " + std::to_string(key.max) + " (&, 0x or $ for hex)";
            }
            return false;
        }
        case Kind::Choice: {
            for (const char* c : key.choices)
                if (text == c) return true;
            *why = "one of ";
            for (size_t i = 0; i < key.choices.size(); i++) *why += (i ? ", " : "") + std::string(key.choices[i]);
            return false;
        }
        case Kind::Bool:
            if (text == "true" || text == "false") return true;
            *why = "true or false";
            return false;
        default: return true;
    }
}

} // namespace debug::attach
