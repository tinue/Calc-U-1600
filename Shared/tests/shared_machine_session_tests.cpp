// Shared/MachineSession against the real ROMs (roms/, run from the repo
// root). Tests SKIP (not fail) when the images are absent.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "MemorySettingsStore.hpp"
#include "Shared/MachineSession.hpp"
#include "SharedTestSupport.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;
#define CHECK(cond) SHARED_CHECK(g_pass, g_fail, cond)

using shared::MachineSession;
using shared::Model;

struct RecordingListener : MachineSession::Listener {
    int replaced = 0;
    int switched = 0;
    int modulesAttached = 0;
    Model lastModel = Model::PC1500A;
    std::string missing;
    void romMissing(const std::string& error, const std::string&) override { missing = error; }
    void machineReplaced() override { ++replaced; }
    void modelSwitched(Model m) override {
        ++switched;
        lastModel = m;
    }
    void attachModulesToFreshMachine() override { ++modulesAttached; }
};

bool romsPresent() {
    return std::ifstream("roms/PC-1500_A04.ROM").good() && std::ifstream("roms/PC1600-P0-B0-new.bin").good();
}

struct Fixture {
    MemorySettingsStore store;
    shared::Settings settings{store};
    std::filesystem::path home = std::filesystem::temp_directory_path() / "calcu-shared-session-test";
    shared::AppPaths paths{settings, shared::HostDirs{home.generic_string(), "roms"}};
    RecordingListener listener;
    MachineSession session{settings, paths, listener};
    ~Fixture() { std::filesystem::remove_all(home); }
};

// Runs `seconds` of emulated time in 60 Hz frames, like a frontend's pacer.
void run(MachineSession& s, double seconds) {
    const auto frame = static_cast<std::uint64_t>(s.clockHz() / 60.0);
    for (int i = 0; i < static_cast<int>(seconds * 60); ++i) s.advance(frame);
}

std::string screen(const MachineSession& s) {
    LcdText text;
    return s.lcdText(&text) ? text.text() : std::string();
}

void test_switch_and_paste(Model model) {
    Fixture f;
    f.session.switchModel(model);
    CHECK(f.listener.missing.empty());
    CHECK(f.session.currentModel() == model);
    CHECK(f.listener.switched == 1 && f.listener.lastModel == model);
    CHECK(f.listener.replaced == 1 && f.listener.modulesAttached == 1);
    CHECK(f.settings.lastUsedModel() == static_cast<int>(model));
    CHECK(f.session.isMachinePoweredOn());

    // A cold PC-1500 shows "NEW0? :CHECK": CL, then NEW0.
    if (model != Model::PC1600) {
        f.session.tapKey("cl");
        run(f.session, 0.5);
        f.session.typeCommand("NEW0");
        run(f.session, 2.0);
    }
    f.session.typeCommand("PRINT 6*7");
    CHECK(f.session.pasteActive());
    run(f.session, 4.0);
    CHECK(!f.session.pasteActive());
    const std::string text = screen(f.session);
    CHECK(text.find("42") != std::string::npos);
    if (text.find("42") == std::string::npos) std::fprintf(stderr, "  screen: %s\n", text.c_str());
}

void test_timed_tap_pc1600() {
    Fixture f;
    f.session.switchModel(Model::PC1600);
    // tapKey on the PC-1600 presses now and releases 30 ms of emulated time
    // later, at the next advance() past it: one "A" on the input line. A
    // key never released would auto-repeat.
    f.session.tapKey("a");
    run(f.session, 1.5);
    const std::string text = screen(f.session);
    CHECK(text.find('A') != std::string::npos);
    CHECK(text.find("AA") == std::string::npos);
    // tapShiftedKey: SHIFT latched, then the base key -- shifted "1" is
    // its second legend, "'".
    f.session.tapShiftedKey("1");
    run(f.session, 1.0);
    CHECK(screen(f.session).find("A'") != std::string::npos);
}

void test_rebuild_keeps_plotter() {
    Fixture f;
    f.session.switchModel(Model::PC1500A);
    f.session.powerCycleAround([&] { CHECK(f.session.attachCE150()); });
    CHECK(f.session.ce150Attached());
    f.session.setPC1500RomRevision(PC1500RomRevision::A04);  // a rebuild, keepPlotter
    CHECK(f.session.ce150Attached());
    f.session.switchModel(Model::PC1600);  // a model switch starts bare
    CHECK(!f.session.ce150Attached());
}

} // namespace

int run_shared_machine_session_tests() {
    if (!romsPresent()) {
        std::printf("shared_machine_session_tests: SKIP (no ROMs in roms/)\n");
        return 0;
    }
    test_switch_and_paste(Model::PC1500A);
    test_switch_and_paste(Model::PC1600);
    test_timed_tap_pc1600();
    test_rebuild_keeps_plotter();
    std::printf("shared_machine_session_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
