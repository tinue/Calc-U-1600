// Shared/Settings, Shared/AppPaths.

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "MemorySettingsStore.hpp"
#include "Shared/AppPaths.hpp"
#include "SharedTestSupport.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;
#define CHECK(cond) SHARED_CHECK(g_pass, g_fail, cond)

namespace fs = std::filesystem;
using shared::Model;
using shared::OpenFolder;

void test_defaults_and_keys() {
    MemorySettingsStore store;
    shared::Settings s(store);
    CHECK(s.traceMaxFileSizeMB() == 50);
    CHECK(s.dapPort() == 32168);
    CHECK(!s.dapEnabled());
    CHECK(s.windowWidth() == 900 && s.windowHeight() == 500);
    CHECK(s.startupModelPreference() == "last");
    CHECK(s.startupModel() == Model::PC1500A);

    s.setDapEnabled(true);
    s.setDapPort(80);  // below 1024: reads as the default
    CHECK(s.dapEnabled() && s.dapPort() == 32168);
    s.setDapPort(40000);
    CHECK(s.dapPort() == 40000);
    CHECK(store.values["debug/dapPort"] == "40000");

    s.setDefaultPresetPath(Model::PC1600, "/x/a.pc1600");
    CHECK(store.values["preset/default/PC1600"] == "/x/a.pc1600");
    s.setDefaultPresetPath(Model::PC1600, "");
    CHECK(store.values.count("preset/default/PC1600") == 0);

    // Samples keeps the pre-split "preset/" group.
    s.setOpenDir(OpenFolder::Samples, "/fixed");
    CHECK(store.values["preset/openDir"] == "/fixed");
    s.setOpenDir(OpenFolder::HostDrive, "/drive");
    CHECK(store.values["hostDrive/openDir"] == "/drive");
}

void test_startup_model() {
    MemorySettingsStore store;
    shared::Settings s(store);
    s.setLastUsedModel(static_cast<int>(Model::PC1600));
    CHECK(s.startupModel() == Model::PC1600);
    s.setStartupModelPreference("PC1500");
    CHECK(s.startupModel() == Model::PC1500);
    s.setStartupModelPreference("last");
    s.setLastUsedModel(7);  // out of range
    CHECK(s.startupModel() == Model::PC1500A);
}

void test_open_start_dir() {
    MemorySettingsStore store;
    shared::Settings s(store);
    CHECK(s.openStartDir(OpenFolder::Basic, "/home") == "/home");
    s.rememberOpenFile(OpenFolder::Basic, "/a/b/prog.bas");
    CHECK(s.lastOpenDir(OpenFolder::Basic) == "/a/b");
    CHECK(s.openStartDir(OpenFolder::Basic, "/home") == "/a/b");
    s.rememberOpenDir(OpenFolder::Basic, "/c/d/");
    CHECK(s.lastOpenDir(OpenFolder::Basic) == "/c/d");
    s.setOpenDir(OpenFolder::Basic, "/fixed");
    CHECK(s.openStartDir(OpenFolder::Basic, "/home") == "/fixed");
}

void test_app_paths() {
    const fs::path root = fs::temp_directory_path() / "calcu-shared-paths-test";
    fs::remove_all(root);
    const std::string home = (root / "home").generic_string();
    MemorySettingsStore store;
    shared::Settings s(store);
    shared::AppPaths paths(s, shared::HostDirs{home, (root / "bundle").generic_string()});

    CHECK(paths.instanceDir() == home + "/Calc-U-1600");
    CHECK(fs::is_directory(home + "/Calc-U-1600"));
    CHECK(paths.instancePathFor("My Card") == home + "/Calc-U-1600/My Card.card.yaml");
    // A separator in the name can't escape the save folder.
    const std::string floppy = paths.floppyInstancePathFor("a/b");
    const std::string dir = home + "/Calc-U-1600/";
    CHECK(floppy.compare(0, dir.size(), dir) == 0);
    CHECK(floppy.find('/', dir.size()) == std::string::npos);
    CHECK(floppy.size() > 12 && floppy.compare(floppy.size() - 12, 12, ".floppy.yaml") == 0);
    CHECK(paths.tapeDir() == home + "/Calc-U-1600/Tapes");
    CHECK(paths.tapePathFor("Side A") == home + "/Calc-U-1600/Tapes/Side A.wav");
    CHECK(paths.displayPath(home + "/Calc-U-1600/x.card.yaml") == "~/Calc-U-1600/x.card.yaml");
    CHECK(paths.displayPath(home + "/Documents/y") == "~/Documents/y");
    CHECK(paths.displayPath(home + "-other/y") == home + "-other/y");
    CHECK(paths.displayPath("/elsewhere") == "/elsewhere");

    s.setInstanceDirOverride((root / "saves").generic_string());
    CHECK(paths.instanceDir() == (root / "saves").generic_string());
    CHECK(fs::is_directory(root / "saves"));
    s.setTapeDirOverride((root / "tapes").generic_string());
    CHECK(paths.tapeDir() == (root / "tapes").generic_string());

    std::string planned;
    CHECK(paths.planNamedSave(MediaKind::Card, "", "", false, &planned) == MediaSaveRefusal::EmptyName);
    CHECK(paths.planNamedSave(MediaKind::Card, "Disk One", "", false, &planned) == MediaSaveRefusal::None);
    CHECK(planned == (root / "saves" / "Disk One.card.yaml").generic_string());

    std::ofstream((root / "saves" / "f.txt").string()) << "x";
    CHECK(shared::isUnderDir((root / "saves" / "f.txt").generic_string(), (root / "saves").generic_string()));
    CHECK(shared::isUnderDir((root / "saves").generic_string(), (root / "saves").generic_string()));
    fs::create_directories(root / "saves-backup");
    CHECK(!shared::isUnderDir((root / "saves-backup").generic_string(), (root / "saves").generic_string()));
    CHECK(!shared::isUnderDir((root / "missing").generic_string(), (root / "saves").generic_string()));
    fs::remove_all(root);
}

} // namespace

int run_shared_settings_tests() {
    test_defaults_and_keys();
    test_startup_model();
    test_open_start_dir();
    test_app_paths();
    std::printf("shared_settings_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
