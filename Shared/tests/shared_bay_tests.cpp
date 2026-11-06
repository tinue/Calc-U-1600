// Shared/ModuleBay, Shared/FloppyBay, Shared/TapeBay against the real ROMs
// and the bundled cards/disks (Qt6/resources/cards), in a throwaway bundle
// and save folder. SKIP without ROMs.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "MemorySettingsStore.hpp"
#include "PC1600/PC1600Machine.hpp"
#include "Shared/FloppyBay.hpp"
#include "Shared/MachineSession.hpp"
#include "Shared/ModuleBay.hpp"
#include "Shared/TapeBay.hpp"
#include "SharedTestSupport.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;
#define CHECK(cond) SHARED_CHECK(g_pass, g_fail, cond)

namespace fs = std::filesystem;
using shared::Model;

bool contains(const std::vector<shared::ModuleBay::ModuleEntry>& v, const std::string& name) {
    for (const auto& e : v)
        if (e.moduleName == name) return true;
    return false;
}

bool contains(const std::vector<std::string>& v, const std::string& name) {
    for (const auto& e : v)
        if (e == name) return true;
    return false;
}

// The app's wiring of the session's bay hooks.
struct BayListener : shared::MachineSession::Listener {
    shared::ModuleBay* modules = nullptr;
    shared::FloppyBay* floppy = nullptr;
    std::string missing;
    void romMissing(const std::string& error, const std::string&) override { missing = error; }
    void attachModulesToFreshMachine() override {
        if (modules) modules->attachAllToFreshMachine();
    }
    void flushFloppy() override {
        if (floppy) floppy->flushPendingPersist();
    }
    void insertSelectedDisk() override {
        if (floppy) floppy->insertSelectedDisk();
    }
};

struct Fixture {
    fs::path root = fs::temp_directory_path() / "calcu-shared-bay-test";
    MemorySettingsStore store;
    shared::Settings settings{store};
    std::unique_ptr<shared::AppPaths> paths;
    BayListener listener;
    std::unique_ptr<shared::MachineSession> session;
    std::unique_ptr<shared::ModuleBay> modules;
    std::unique_ptr<shared::FloppyBay> floppy;
    std::vector<std::string> errors;

    Fixture() {
        fs::remove_all(root);
        const fs::path bundle = root / "bundle";
        fs::create_directories(bundle);
        for (const auto& dir : {fs::path("roms"), fs::path("Qt6/resources/cards")})
            for (const auto& f : fs::directory_iterator(dir))
                if (f.is_regular_file()) fs::copy_file(f.path(), bundle / f.path().filename());
        paths = std::make_unique<shared::AppPaths>(
            settings, shared::HostDirs{(root / "home").generic_string(), bundle.generic_string()});
        session = std::make_unique<shared::MachineSession>(settings, *paths, listener);
        auto onError = [this](const std::string& e) { errors.push_back(e); };
        modules = std::make_unique<shared::ModuleBay>(*session, *paths, shared::ModuleBay::Events{{}, onError});
        floppy = std::make_unique<shared::FloppyBay>(*session, *paths, shared::FloppyBay::Events{onError});
        listener.modules = modules.get();
        listener.floppy = floppy.get();
    }
    ~Fixture() {
        floppy.reset();
        modules.reset();
        session.reset();
        fs::remove_all(root);
    }
};

void test_module_bay() {
    Fixture f;
    const auto lists = f.modules->moduleLists(CardHost::PC1600Slot1);
    CHECK(contains(lists.templates, "CE-1600M"));
    CHECK(!contains(lists.templates, "CE-1601M"));  // Slot 2 only
    CHECK(lists.instances.empty());

    f.modules->selectModule(1, "CE-1600M");
    f.session->switchModel(Model::PC1600);
    CHECK(f.errors.empty());
    CHECK(f.session->pc1600()->memory().slotModuleName(1) == "CE-1600M");
    CHECK(f.modules->canNameAndSave(1));
    CHECK(!f.modules->slotHasInstanceFile(1));

    std::string error;
    CHECK(!f.modules->nameAndSave(1, "CE-1600M", &error));  // a template's name
    CHECK(f.modules->nameAndSave(1, "My RAM", &error));
    const std::string saved = f.paths->instancePathFor("My RAM");
    CHECK(fs::exists(saved));
    CHECK(f.modules->selectedModuleName(1) == "My RAM");
    CHECK(f.modules->slotHasInstanceFile(1));
    CHECK(!f.modules->canNameAndSave(1));
    CHECK(contains(f.modules->moduleLists(CardHost::PC1600Slot1).instances, "My RAM"));
    CHECK(!f.modules->nameAndSave(1, "Other", &error));  // already saved: autosave

    // A rebuild re-attaches the saved instance from its file.
    f.session->switchModel(Model::PC1600, /*keepPlotter=*/true);
    CHECK(f.session->pc1600()->memory().slotModuleName(1) == "My RAM");
    // A model switch clears the slots.
    f.modules->onModelChanged();
    f.session->switchModel(Model::PC1500A);
    CHECK(f.modules->selectedModuleName(1).empty());
}

void test_floppy_bay() {
    Fixture f;
    const auto lists = f.floppy->diskLists();
    CHECK(contains(lists.templates, "Formatted"));
    f.session->switchModel(Model::PC1600);
    f.floppy->selectDisk("Formatted");  // no drive yet: selection only
    CHECK(f.floppy->selectedDiskName() == "Formatted");
    f.session->powerCycleAround([&] { CHECK(f.session->attachCE1600P()); });  // inserts the selected disk
    PC1600Machine* m = f.session->pc1600();
    CHECK(m->ce1600fHasDisk());
    CHECK(f.floppy->canNameAndSave());
    f.floppy->toggleSide();
    CHECK(f.floppy->side() == 1);

    std::string error;
    CHECK(f.floppy->nameAndSave("Work", &error));
    const std::string saved = f.paths->floppyInstancePathFor("Work");
    CHECK(fs::exists(saved));
    CHECK(!f.floppy->canNameAndSave());

    // The drive writes: autosaved once the debounce has passed, not before.
    fs::remove(saved);
    std::vector<std::uint8_t> image = m->ce1600fDiskImage();
    image[100] ^= 0xFF;
    m->ce1600fLoadImage(image.data(), image.size());
    f.floppy->markDirty();
    f.floppy->tick(10.0);
    f.floppy->tick(10.3);
    CHECK(!fs::exists(saved));
    f.floppy->tick(10.6);
    CHECK(fs::exists(saved));
}

void test_tape_bay() {
    Fixture f;
    int changed = 0;
    shared::TapeBay tapes(*f.session, *f.paths, shared::TapeBay::Events{[&] { ++changed; }, {}});
    f.session->switchModel(Model::PC1500A);
    CHECK(tapes.tapeNames().empty());
    std::ofstream(f.paths->tapePathFor("b side")) << "x";
    std::ofstream(f.paths->tapePathFor("A side")) << "x";
    const auto names = tapes.tapeNames();
    CHECK(names.size() == 2 && names[0] == "A side" && names[1] == "b side");
    CHECK(tapes.exists("A side"));
    CHECK(!tapes.recordNew("   "));
    CHECK(tapes.recordNew(" New "));
    CHECK(tapes.currentName() == "New");
    std::string error;
    CHECK(tapes.selectForPlay("", &error));
    CHECK(tapes.currentName().empty());
}

} // namespace

int run_shared_bay_tests() {
    if (!std::ifstream("roms/PC1600-P0-B0-new.bin").good()) {
        std::printf("shared_bay_tests: SKIP (no ROMs in roms/)\n");
        return 0;
    }
    test_module_bay();
    test_floppy_bay();
    test_tape_bay();
    std::printf("shared_bay_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
