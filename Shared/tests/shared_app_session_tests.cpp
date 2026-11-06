// Shared/AppSession: operations run cooperatively from tick() (as a
// browser frontend would) and with runNow() (as the Qt app does). SKIP
// without ROMs.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "MemorySettingsStore.hpp"
#include "PC1600/PC1600Machine.hpp"
#include "Shared/AppSession.hpp"
#include "SharedTestSupport.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;
#define CHECK(cond) SHARED_CHECK(g_pass, g_fail, cond)

namespace fs = std::filesystem;
using shared::AppSession;
using shared::Model;
using shared::OpResult;

struct RecordingPlatform : AppSession::Platform {
    std::string missing;
    int armed = 0;
    std::vector<bool> busy;
    std::vector<std::string> attachFailures;
    int logLines = 0;
    void romMissing(const std::string& error, const std::string&) override { missing = error; }
    void presetArmed() override { ++armed; }
    void busyChanged(bool b) override { busy.push_back(b); }
    void presetLog(const std::string&) override { ++logLines; }
    void attachFailed(const std::string& device, const std::string& reason) override {
        attachFailures.push_back(device + ": " + reason);
    }
};

struct Fixture {
    fs::path root = fs::temp_directory_path() / "calcu-shared-app-test";
    MemorySettingsStore store;
    shared::Settings settings{store};
    std::unique_ptr<shared::AppPaths> paths;
    RecordingPlatform platform;
    std::unique_ptr<AppSession> app;

    Fixture() {
        fs::remove_all(root);
        const fs::path bundle = root / "bundle";
        fs::create_directories(bundle);
        for (const auto& dir : {fs::path("roms"), fs::path("Qt6/resources/cards")})
            for (const auto& f : fs::directory_iterator(dir))
                if (f.is_regular_file()) fs::copy_file(f.path(), bundle / f.path().filename());
        paths = std::make_unique<shared::AppPaths>(
            settings, shared::HostDirs{(root / "home").generic_string(), bundle.generic_string()});
        app = std::make_unique<AppSession>(settings, *paths, platform);
    }
    ~Fixture() {
        app.reset();
        fs::remove_all(root);
    }
};

std::string screen(AppSession& app) {
    LcdText text;
    return app.machine().lcdText(&text) ? text.text() : std::string();
}

void test_cooperative_preset() {
    Fixture f;
    f.settings.setStartupModelPreference("PC1500A");
    f.app->start();
    CHECK(f.platform.missing.empty());
    CHECK(f.app->machine().currentModel() == Model::PC1500A);

    // The default PC-1600 preset (CE-1600P + floppy, two slot cards, a
    // dozen typed lines), a tick at a time.
    bool finished = false;
    OpResult result;
    CHECK(f.app->start(f.app->loadPresetJob("Core/tests/fixtures/presets/startup-pc1600.pc1600"), [&](const OpResult& r) {
        finished = true;
        result = r;
    }));
    CHECK(f.app->busy());
    // A second operation is refused while one runs.
    CHECK(!f.app->start(f.app->resetJob(false)));
    int ticks = 0;
    while (!finished && ticks < 100000) {
        f.app->tick();
        ++ticks;
    }
    CHECK(finished);
    CHECK(result.ok);
    if (!result.ok) std::fprintf(stderr, "  preset: %s\n", result.error.c_str());
    CHECK(ticks > 10);  // it really ran in slices
    CHECK(!f.app->busy());
    CHECK((f.platform.busy == std::vector<bool>{true, false}));
    CHECK(f.platform.armed == 1);
    CHECK(f.platform.logLines > 10);
    CHECK(f.app->machine().currentModel() == Model::PC1600);
    CHECK(f.app->machine().ce1600pAttached());
    CHECK(f.app->modules().selectedModuleName(1) == "CE-1600M");
    CHECK(f.app->modules().selectedModuleName(2) == "CE-1601M");
    CHECK(f.app->floppy().selectedDiskName() == "Formatted");
    CHECK(screen(*f.app).find("Memory:") != std::string::npos);
}

void test_run_now_and_selection() {
    Fixture f;
    f.app->start();
    int pumps = 0;
    OpResult r = f.app->runNow(f.app->selectModelJob(Model::PC1600), [&] { ++pumps; });
    CHECK(r.ok);
    CHECK(pumps > 0);
    CHECK(f.app->machine().currentModel() == Model::PC1600);
    CHECK(f.settings.lastUsedModel() == static_cast<int>(Model::PC1600));

    r = f.app->runNow(f.app->togglePeripheralJob(shared::Peripheral::CE1600P));
    CHECK(r.ok && f.app->machine().ce1600pAttached());
    r = f.app->runNow(f.app->togglePeripheralJob(shared::Peripheral::CE1600P));
    CHECK(r.ok && !f.app->machine().ce1600pAttached());
    CHECK(f.platform.attachFailures.empty());

    // The default preset of another model is refused, touching nothing.
    r = f.app->runNow(f.app->loadDefaultPresetJob("Core/tests/fixtures/presets/startup-pc1500a.pc1500a", Model::PC1600));
    CHECK(!r.ok);
    CHECK(f.app->machine().currentModel() == Model::PC1600);
    // A preset that doesn't parse.
    r = f.app->runNow(f.app->loadPresetJob("Core/tests/fixtures/presets/no-such.pc1600"));
    CHECK(!r.ok && !r.error.empty());
}

} // namespace

int run_shared_app_session_tests() {
    if (!std::ifstream("roms/PC1600-P0-B0-new.bin").good()) {
        std::printf("shared_app_session_tests: SKIP (no ROMs in roms/)\n");
        return 0;
    }
    test_cooperative_preset();
    test_run_now_and_selection();
    std::printf("shared_app_session_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
