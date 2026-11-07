#include "Shared/PresetCatalog.hpp"

#include <fstream>
#include <sstream>

#include "Preset/PresetFile.hpp"
#include "Shared/AppPaths.hpp"
#include "Shared/Utf8Path.hpp"
#include "Yaml.hpp"

namespace shared {
namespace {

std::optional<Model> modelOf(const std::string& canonical) {
    if (canonical == "PC-1500") return Model::PC1500;
    if (canonical == "PC-1500A") return Model::PC1500A;
    if (canonical == "PC-1600") return Model::PC1600;
    return std::nullopt;
}

// One `- file:` entry of bundled.yaml's `list` (with its `group:`).
struct ListedFile {
    std::string group;
    std::string file;
};

bool readList(const std::string& path, const char* list, std::vector<ListedFile>* out, std::string* error) {
    std::ifstream in(toPath(path), std::ios::binary);
    if (!in) {
        *error = "cannot read " + path;
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    YamlNode root;
    if (!parseYaml(text.str(), &root, error)) return false;
    const YamlNode* items = root.find(list);
    if (!items) return true;
    if (!items->isSeq()) {
        *error = std::string("'") + list + "' must be a list";
        return false;
    }
    for (const YamlNode& item : items->seq) {
        ListedFile f;
        const YamlNode* file = item.find("file");
        if (!file || !file->asString(&f.file, error)) {
            *error = "line " + std::to_string(item.line) + ": an entry needs 'file:'";
            return false;
        }
        if (const YamlNode* group = item.find("group"))
            if (!group->asString(&f.group, error)) return false;
        out->push_back(std::move(f));
    }
    return true;
}

std::vector<PresetEntry> listed(const AppPaths& paths, const char* list, std::string* error) {
    std::vector<ListedFile> files;
    std::string err;
    if (!readList(joinPath(paths.bundledExamplesDir(), "bundled.yaml"), list, &files, &err)) {
        if (error) *error = err;
        return {};
    }
    std::vector<PresetEntry> out;
    for (const ListedFile& f : files) {
        PresetCatalog catalog(paths);
        std::optional<PresetEntry> e = catalog.describe(kExamplePresetPrefix + f.file);
        if (!e) {
            if (error) *error += (error->empty() ? "" : "\n") + f.file + ": can't be read as a preset";
            continue;
        }
        e->group = f.group;
        out.push_back(std::move(*e));
    }
    return out;
}

} // namespace

std::optional<PresetEntry> PresetCatalog::describe(const std::string& ref) const {
    const std::string path = m_paths.resolvePresetRef(ref);
    PresetHeader header;
    std::string err;
    if (!readPresetHeader(path, &header, &err)) return std::nullopt;
    const std::optional<Model> model = modelOf(header.model);
    if (!model) return std::nullopt;
    PresetEntry e;
    e.ref = ref;
    e.path = path;
    e.model = *model;
    e.name = presetDisplayName(header.presetName, path);
    e.description = header.description;
    e.hasName = !header.presetName.empty();
    return e;
}

std::vector<PresetEntry> PresetCatalog::defaults(Model model) const {
    std::vector<PresetEntry> out;
    for (PresetEntry& e : listed(m_paths, "defaults", nullptr))
        if (e.model == model) out.push_back(std::move(e));
    return out;
}

std::vector<PresetEntry> PresetCatalog::examples() const { return listed(m_paths, "examples", nullptr); }

std::vector<PresetEntry> PresetCatalog::all(std::string* error) const {
    std::vector<PresetEntry> out = listed(m_paths, "defaults", error);
    std::string more;
    for (PresetEntry& e : listed(m_paths, "examples", error ? &more : nullptr)) out.push_back(std::move(e));
    if (error && !more.empty()) *error += (error->empty() ? "" : "\n") + more;
    return out;
}

std::vector<std::string> presetNameProblems(const std::vector<PresetEntry>& entries) {
    std::vector<std::string> problems;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const PresetEntry& e = entries[i];
        if (!e.hasName) problems.push_back(e.ref + ": no preset-name:");
        if (e.description.empty()) problems.push_back(e.ref + ": no description:");
        if (e.name.size() > kPresetNameMaxLength)
            problems.push_back(e.ref + ": preset-name \"" + e.name + "\" is longer than " +
                               std::to_string(kPresetNameMaxLength) + " characters");
        for (std::size_t j = 0; j < i; ++j) {
            if (entries[j].ref == e.ref) problems.push_back(e.ref + ": listed twice");
            else if (entries[j].model == e.model && entries[j].name == e.name)
                problems.push_back(e.ref + ": same preset-name \"" + e.name + "\" as " + entries[j].ref);
        }
    }
    return problems;
}

} // namespace shared
