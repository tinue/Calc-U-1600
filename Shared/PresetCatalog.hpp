#pragma once
#include <optional>
#include <string>
#include <vector>

#include "Shared/MachineModel.hpp"

namespace shared {

class AppPaths;

// ── PresetCatalog: the presets the app offers by name ────────────────────
//
// The two curated lists of examples/bundled.yaml -- `defaults` (Settings ▸
// Default presets) and `examples` (File ▸ Examples) -- and the name and
// description of any other preset file. A preset carries its own short
// name (`preset-name:`, shown once chosen) and description (the line lists
// and menus show); the list only says which files and in which group.
// Read on each call: a handful of small header reads.
struct PresetEntry {
    std::string group;        // `examples` only; "" in `defaults`
    std::string ref;          // what Settings stores and loadPresetJob() takes ("examples:...", or a path)
    Model model = Model::PC1600;
    std::string name;         // `preset-name:`, else the file name without extension
    std::string description;  // `description:`, else ""
    bool hasName = false;     // the file has a `preset-name:` of its own

    // What a list or menu shows: the description, else the name.
    const std::string& label() const { return description.empty() ? name : description; }
};

class PresetCatalog {
public:
    explicit PresetCatalog(const AppPaths& paths) : m_paths(paths) {}

    // Settings ▸ Default presets: the bundled ones for `model`.
    std::vector<PresetEntry> defaults(Model model) const;
    // The Settings pick list: defaults(model), plus `current` (the stored
    // setting) when it is set and not one of them -- described, or, if it
    // can't be read, by its file name alone.
    std::vector<PresetEntry> defaultChoices(Model model, const std::string& current) const;
    // File ▸ Examples, in list order.
    std::vector<PresetEntry> examples() const;
    // Both lists, unfiltered -- for the bundle check.
    std::vector<PresetEntry> all(std::string* error = nullptr) const;

    // Any preset -- an "examples:" ref or a path; none if it can't be read.
    std::optional<PresetEntry> describe(const std::string& ref) const;

private:
    const AppPaths& m_paths;
};

// What's wrong with a bundled list, one line per problem: a preset without
// `preset-name:` / `description:`, a short name longer than
// kPresetNameMaxLength, two presets of one model with the same name. Empty
// = fine.
constexpr std::size_t kPresetNameMaxLength = 18;
std::vector<std::string> presetNameProblems(const std::vector<PresetEntry>& entries);

} // namespace shared
