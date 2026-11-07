// Shared/PresetCatalog over the repo's examples/bundled.yaml: every listed
// preset exists, has its own short name and description, and the names
// tell the presets of one model apart; plus the "examples:" refs.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

#include "MemorySettingsStore.hpp"
#include "Shared/AppPaths.hpp"
#include "Shared/PresetCatalog.hpp"
#include "SharedTestSupport.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;
#define CHECK(cond) SHARED_CHECK(g_pass, g_fail, cond)

namespace fs = std::filesystem;
using shared::Model;
using shared::PresetEntry;

struct Fixture {
    MemorySettingsStore store;
    shared::Settings settings{store};
    shared::AppPaths paths{settings, shared::HostDirs{fs::temp_directory_path().generic_string(), "roms", "examples"}};
    shared::PresetCatalog catalog{paths};
};

void printProblems(const std::vector<std::string>& problems) {
    for (const auto& p : problems) std::fprintf(stderr, "  bundled.yaml: %s\n", p.c_str());
}

// The bundle check: what Qt6 and the web client offer by name.
void test_bundled_lists() {
    Fixture f;
    std::string error;
    const std::vector<PresetEntry> all = f.catalog.all(&error);
    if (!error.empty()) std::fprintf(stderr, "  bundled.yaml: %s\n", error.c_str());
    CHECK(error.empty());

    std::vector<PresetEntry> defaults;
    for (Model m : {Model::PC1500, Model::PC1500A, Model::PC1600}) {
        const auto forModel = f.catalog.defaults(m);
        CHECK(!forModel.empty());
        for (const auto& e : forModel) CHECK(e.model == m);
        defaults.insert(defaults.end(), forModel.begin(), forModel.end());
    }
    const std::vector<PresetEntry> examples = f.catalog.examples();
    CHECK(examples.size() > 10);
    CHECK(defaults.size() + examples.size() == all.size());

    auto problems = shared::presetNameProblems(defaults);
    printProblems(problems);
    CHECK(problems.empty());
    problems = shared::presetNameProblems(examples);
    printProblems(problems);
    CHECK(problems.empty());

    // A default preset is not an example too.
    std::set<std::string> defaultRefs;
    for (const auto& e : defaults) defaultRefs.insert(e.ref);
    for (const auto& e : examples) {
        CHECK(!defaultRefs.count(e.ref));
        CHECK(!e.group.empty());
    }
}

void test_describe() {
    Fixture f;
    const auto hanoi = f.catalog.describe("examples:basic/hanoi.pc1600");
    CHECK(hanoi && hanoi->name == "Hanoi" && hanoi->model == Model::PC1600 && !hanoi->description.empty());
    CHECK(hanoi && hanoi->path == "examples/basic/hanoi.pc1600");
    CHECK(f.paths.resolvePresetRef("/x/y.pc1600") == "/x/y.pc1600");

    // A preset without a name of its own: the file name stands in.
    const fs::path plain = fs::temp_directory_path() / "calcu-catalog-plain.pc1500a";
    std::ofstream(plain) << "format-version: 1\nmodel: PC-1500A\n";
    const auto e = f.catalog.describe(plain.generic_string());
    CHECK(e && e->name == "calcu-catalog-plain" && !e->hasName && e->description.empty());
    fs::remove(plain);
    CHECK(!f.catalog.describe("examples:no/such.pc1600"));
}

void test_name_problems() {
    PresetEntry a;
    a.ref = "a";
    a.model = Model::PC1600;
    a.name = "Same";
    a.description = "A";
    a.hasName = true;
    PresetEntry b = a;
    b.ref = "b";
    CHECK(shared::presetNameProblems({a}).empty());
    CHECK(shared::presetNameProblems({a, b}).size() == 1);  // same name, same model
    b.model = Model::PC1500;
    CHECK(shared::presetNameProblems({a, b}).empty());      // other model: fine
    b.name = "A name much too long";
    b.hasName = false;
    b.description.clear();
    CHECK(shared::presetNameProblems({b}).size() == 3);
}

} // namespace

int run_shared_preset_catalog_tests() {
    test_bundled_lists();
    test_describe();
    test_name_problems();
    std::printf("shared_preset_catalog_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
