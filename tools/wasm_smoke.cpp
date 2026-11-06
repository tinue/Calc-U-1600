// WebAssembly smoke test for Core + Shared (tools/build_wasm_check.sh):
// built with Emscripten, run under node from the repo root (NODERAWFS, so
// roms/ and the bundled cards are read straight from disk). Boots both
// models through shared::AppSession and runs each default preset the way a
// browser frontend would -- start() it, then tick() until done, no
// blocking -- and checks the LCD. Exit 0 = pass; 2 = no ROMs (skip).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "Shared/AppSession.hpp"
#include "Shared/Pacer.hpp"
#include "Shared/tests/MemorySettingsStore.hpp"

namespace {

namespace fs = std::filesystem;

class ConsolePlatform : public shared::AppSession::Platform {
public:
    void romMissing(const std::string& error, const std::string& searchedIn) override {
        std::fprintf(stderr, "ROM missing: %s (searched %s)\n", error.c_str(), searchedIn.c_str());
    }
    void bayError(shared::Bay, const std::string& text) override { std::fprintf(stderr, "bay: %s\n", text.c_str()); }
    void attachFailed(const std::string& device, const std::string& reason) override {
        std::fprintf(stderr, "attach %s: %s\n", device.c_str(), reason.c_str());
    }
};

std::string screen(shared::AppSession& app) {
    LcdText text;
    return app.machine().lcdText(&text) ? text.text() : std::string();
}

// Runs a preset cooperatively; false if it failed or never finished.
bool runPreset(shared::AppSession& app, const std::string& path, int* ticks) {
    bool finished = false;
    shared::OpResult result;
    if (!app.start(app.loadPresetJob(path), [&](const shared::OpResult& r) {
            finished = true;
            result = r;
        })) {
        std::fprintf(stderr, "could not start %s\n", path.c_str());
        return false;
    }
    *ticks = 0;
    while (!finished && *ticks < 1000000) {
        app.tick();
        ++*ticks;
    }
    if (!finished) return false;
    if (!result.ok) std::fprintf(stderr, "%s: %s\n", path.c_str(), result.error.c_str());
    return result.ok;
}

} // namespace

int main() {
    if (!std::ifstream("roms/PC1600-P0-B0-new.bin").good() || !std::ifstream("roms/PC-1500_A04.ROM").good()) {
        std::printf("wasm_smoke: SKIP (no ROMs in roms/; run from the repo root)\n");
        return 2;
    }
    // A bundle folder like the app's: ROMs and the bundled cards/disks.
    const fs::path root = fs::temp_directory_path() / "calcu-wasm-smoke";
    fs::remove_all(root);
    fs::create_directories(root / "bundle");
    for (const auto& dir : {fs::path("roms"), fs::path("Qt6/resources/cards")})
        for (const auto& f : fs::directory_iterator(dir))
            if (f.is_regular_file()) fs::copy_file(f.path(), root / "bundle" / f.path().filename());

    MemorySettingsStore store;
    shared::Settings settings(store);
    shared::AppPaths paths(settings, shared::HostDirs{(root / "home").generic_string(), (root / "bundle").generic_string()});
    ConsolePlatform platform;
    shared::AppSession app(settings, paths, platform);
    app.start();

    int failures = 0;
    struct Case {
        const char* preset;
        const char* expect;
    };
    for (const Case& c : {Case{"Core/tests/fixtures/presets/startup-pc1600.pc1600", "Memory:"},
                          Case{"Core/tests/fixtures/presets/hanoi.pc1600", ""},
                          Case{"Core/tests/fixtures/presets/startup-pc1500a.pc1500a", ""}}) {
        int ticks = 0;
        const bool ok = runPreset(app, c.preset, &ticks);
        const std::string lcd = screen(app);
        const bool seen = lcd.find(c.expect) != std::string::npos;
        std::printf("%s %s (%d ticks)\n%s\n", ok && seen ? "ok  " : "FAIL", c.preset, ticks, lcd.c_str());
        if (!ok || !seen) ++failures;
    }
    // Typing through the paste queue, in paced frames.
    app.machine().typeCommand("PRINT 6*7");
    shared::runEmulatedSeconds(app.machine(), 4.0);
    const bool typed = screen(app).find("42") != std::string::npos;
    std::printf("%s PRINT 6*7\n%s\n", typed ? "ok  " : "FAIL", screen(app).c_str());
    if (!typed) ++failures;

    fs::remove_all(root);
    std::printf("wasm_smoke: %s\n", failures == 0 ? "all passed" : "FAILED");
    return failures == 0 ? 0 : 1;
}
