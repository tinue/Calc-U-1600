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

// tapeStatus() is what the tape bay saw at the last frame -- and an
// operation ends with one, so it is current right after.
void test_tape_status_follows_frames() {
    Fixture f;
    f.settings.setStartupModelPreference("PC1500A");
    f.app->start();
    CHECK(f.app->runNow(f.app->togglePeripheralJob(shared::Peripheral::CE150)).ok);
    CHECK(f.app->machine().tapeInterfaceAttached());
    CHECK(f.app->tapeStatus().mode == TapeDeck::Mode::Empty);
    CHECK(f.app->tapes().recordNew("side a"));
    CHECK(f.app->tapeStatus().mode == TapeDeck::Mode::Empty);  // not seen yet
    f.app->tick();
    CHECK(f.app->tapeStatus().mode == TapeDeck::Mode::Record);
    std::string error;
    CHECK(f.app->tapes().selectForPlay({}, &error));
    CHECK(f.app->runNow(f.app->resetJob(false)).ok);
    CHECK(f.app->tapeStatus().mode == TapeDeck::Mode::Empty);
}

// controlState(): each model's own peripherals on the bar, the CE-1600P
// excluding the CE-150 / CE-158, the floppy and tape rows following the
// attached boxes.
void test_control_state() {
    Fixture f;
    f.settings.setStartupModelPreference("PC1500");
    f.app->start();
    shared::ControlState s = f.app->controlState();
    CHECK(s.model == Model::PC1500 && s.slotCount == 1);
    CHECK(s.ce150.onControlBar && s.ce158.onControlBar && !s.ce1600p.offered);
    CHECK(s.ce150.enabled && s.ce158.enabled);
    CHECK(s.pc1500RomMenu && !s.pc1600RomMenu && !s.ce1600pRomMenu);
    CHECK(!s.floppyShown && !s.tapeShown && !s.hostDriveOffered);
    CHECK(s.paper == shared::PaperKind::None && !s.printerShown);

    CHECK(f.app->runNow(f.app->togglePeripheralJob(shared::Peripheral::CE150)).ok);
    s = f.app->controlState();
    CHECK(s.ce150.attached && s.tapeShown && s.paper == shared::PaperKind::CE150);

    CHECK(f.app->runNow(f.app->selectModelJob(Model::PC1600)).ok);
    s = f.app->controlState();
    CHECK(s.slotCount == 2 && s.floppyShown && !s.floppyEnabled);
    CHECK(s.ce1600p.offered && s.ce1600p.onControlBar && s.ce1600p.enabled);
    CHECK(!s.ce150.onControlBar && s.ce150.offered && !s.pc1500RomMenu && s.pc1600RomMenu);
    CHECK(s.hostDriveOffered && s.hostDriveDir.empty());

    CHECK(f.app->runNow(f.app->togglePeripheralJob(shared::Peripheral::CE1600P)).ok);
    s = f.app->controlState();
    CHECK(s.ce1600p.attached && s.floppyEnabled && s.tapeShown && s.paper == shared::PaperKind::CE1600P);
    CHECK(!s.ce150.enabled && !s.ce158.enabled);
    CHECK(&s.peripheral(shared::Peripheral::CE158) == &s.ce158);
}

// A frontend without host directories never offers the host drive.
void test_control_state_without_host_directories() {
    struct NoDirsPlatform : RecordingPlatform {
        bool hostDirectories() const override { return false; }
    };
    Fixture f;
    NoDirsPlatform platform;
    f.app.reset();
    f.app = std::make_unique<AppSession>(f.settings, *f.paths, platform);
    f.settings.setStartupModelPreference("PC1600");
    f.app->start();
    CHECK(!f.app->controlState().hostDriveOffered);
    f.app.reset();
}

struct CountingSink : shared::AudioSink {
    std::size_t written = 0;
    bool start(int, int) override { return true; }
    void stop() override {}
    std::size_t room() const override { return 1 << 20; }
    void write(const std::int16_t*, std::size_t count) override { written += count; }
    bool ranDry() const override { return false; }
};

// tick() plays the buzzer in real time and throws turbo audio away.
void test_audio_follows_tick() {
    Fixture f;
    CountingSink sink;
    f.app->attachAudio(sink, {});
    f.settings.setStartupModelPreference("PC1600");
    f.app->start();
    f.app->machine().typeCommand("BEEP 50");
    CHECK(f.app->runUntilPasteDone(5.0));
    CHECK(sink.written == 0);  // scripted: not real time
    const std::int64_t deadline = shared::Pacer::steadyNowNs() + 3'000'000'000LL;
    while (sink.written == 0 && shared::Pacer::steadyNowNs() < deadline) f.app->tick();
    CHECK(sink.written > 0);
    f.app->pacer().setTurbo(true);
    f.app->tick();  // drops what the paced ticks left
    const std::size_t before = sink.written;
    for (int i = 0; i < 5; ++i) f.app->tick();
    CHECK(sink.written == before);
}

} // namespace

int run_shared_app_session_tests() {
    if (!std::ifstream("roms/PC1600-P0-B0-new.bin").good()) {
        std::printf("shared_app_session_tests: SKIP (no ROMs in roms/)\n");
        return 0;
    }
    test_cooperative_preset();
    test_run_now_and_selection();
    test_tape_status_follows_frames();
    test_audio_follows_tick();
    test_control_state();
    test_control_state_without_host_directories();
    std::printf("shared_app_session_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
