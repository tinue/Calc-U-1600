// Shared/AppSession: operations run cooperatively from tick() (as a
// browser frontend would) and with runNow() (as the Qt app does). SKIP
// without ROMs.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "MemorySettingsStore.hpp"
#include "PC1500/PC1500Machine.hpp"
#include "PC1600/PC1600Machine.hpp"
#include "Shared/AppSession.hpp"
#include "Shared/CardYamlDump.hpp"
#include "Shared/MachineCodeLoad.hpp"
#include "Shared/MediaLibrary.hpp"
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
            settings, shared::HostDirs{(root / "home").generic_string(), bundle.generic_string(),
                                       fs::absolute("examples").generic_string()});
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
    CHECK(f.app->tapes().recordNew("side a", nullptr));
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

std::vector<std::uint8_t> readBytes(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The same tape twice in one WAV: its data chunk's samples doubled.
std::vector<std::uint8_t> doubledTape(const std::vector<std::uint8_t>& wav) {
    for (std::size_t i = 12; i + 8 <= wav.size();) {
        const std::uint32_t size = wav[i + 4] | wav[i + 5] << 8 | wav[i + 6] << 16 | std::uint32_t(wav[i + 7]) << 24;
        if (std::equal(wav.begin() + i, wav.begin() + i + 4, "data")) {
            std::vector<std::uint8_t> out(wav.begin(), wav.begin() + i + 8 + size);
            out.insert(out.end(), wav.begin() + i + 8, wav.begin() + i + 8 + size);
            const std::uint32_t doubled = size * 2, riff = static_cast<std::uint32_t>(out.size() - 8);
            for (int b = 0; b < 4; ++b) {
                out[i + 4 + b] = static_cast<std::uint8_t>(doubled >> (8 * b));
                out[4 + b] = static_cast<std::uint8_t>(riff >> (8 * b));
            }
            return out;
        }
        i += 8 + size + (size & 1);
    }
    return {};
}

// programChoice(): a plain file is its own image, a one-file tape its
// file, a several-file tape a list to pick from.
void test_program_choice() {
    const std::vector<std::uint8_t> plain = {1, 2, 3};
    shared::ProgramChoice c = shared::programChoice(plain);
    CHECK(!c.needsChoice() && c.images.size() == 1 && c.images[0] == plain && c.names.empty());

    const std::vector<std::uint8_t> wav = readBytes("Core/tests/fixtures/tape/pc1500_tape.wav");
    c = shared::programChoice(wav);
    CHECK(!c.needsChoice() && c.images.size() == 1 && c.images[0] != wav);

    c = shared::programChoice(doubledTape(wav));
    CHECK(c.needsChoice() && c.names.size() == 2);
    CHECK(c.names.size() == 2 && c.names[0].rfind("1. TAPEFIX", 0) == 0 && c.names[1].rfind("2. ", 0) == 0);
}

// MachineCodeLoad: a headerless file asks for an address, checked as
// typed; the load gives the summary and the NEW / CALL advice.
void test_machine_code_load() {
    Fixture f;
    f.settings.setStartupModelPreference("PC1500A");
    f.app->start();
    const std::vector<std::uint8_t> code = {0x9A, 0x9A, 0x9A, 0x9A};  // RTN x4
    shared::MachineCodeLoad load = shared::MachineCodeLoad::plan(f.app->machine(), code);
    CHECK(load.error().empty() && load.needsAddress() && load.length() == 4);
    CHECK(load.cpuNote().empty() && load.addressPlaceholder() == "hex, e.g. &7C01");
    shared::MachineCodeLoad::AddressCheck check = load.setAddress("zz");
    CHECK(!check.valid && check.hint == "Not a hex address (&0000-&FFFF).");
    CHECK(!load.setAddress("").valid && load.setAddress("").hint.empty());
    CHECK(!load.setAddress("&FFFE").valid);
    CHECK(load.setAddress("&7C01").valid);
    CHECK(load.request().addr == 0x7C01 && load.request().payload == code);
    CHECK(f.app->loadMachineCode(load.request()).ok);
    shared::MachineCodeLoad::Loaded loaded = load.loaded(f.app->machine());
    CHECK(loaded.summary == "Loaded 4 bytes at &7C01\u2013&7C04");
    CHECK(loaded.advice.callCommand == "CALL &7C01");

    CHECK(f.app->runNow(f.app->selectModelJob(Model::PC1600)).ok);
    load = shared::MachineCodeLoad::plan(f.app->machine(), code);
    CHECK(load.error().empty() && load.needsAddress());
    CHECK(load.defaultAddressText() == "&C0C5" && !load.cpuNote().empty());
    check = load.setAddress(load.defaultAddressText());
    CHECK(check.valid && check.hint.empty());
    CHECK(load.request().addr == 0xC0C5 && load.request().slot == 0 && load.request().bank == 0);
    CHECK(f.app->loadMachineCode(load.request()).ok);
    loaded = load.loaded(f.app->machine());
    CHECK(loaded.summary == "Loaded 4 bytes at &C0C5\u2013&C0C8 (S0)");
    CHECK(loaded.advice.callCommand == "CALL &C0C5");
    // Into the work area: a warning, still loadable.
    check = load.setAddress("&F800");
    CHECK(check.valid && !check.hint.empty());
}

const shared::MediaItem* findItem(const std::vector<shared::MediaItem>& items, const std::string& name) {
    for (const auto& item : items)
        if (item.name == name) return &item;
    return nullptr;
}

// MediaLibrary: the save folder's cards, disks and tapes -- listed with
// their in-use state, exported, deleted (not while in use), imported back
// (not over an existing name).
void test_media_library() {
    Fixture f;
    f.settings.setStartupModelPreference("PC1600");
    f.app->start();
    shared::MediaLibrary library(*f.app);
    CHECK(library.items().empty());

    CHECK(f.app->runNow(f.app->selectModuleJob(1, "CE-1600M")).ok);
    std::string error;
    CHECK(f.app->modules().nameAndSave(1, "Work", &error));
    std::vector<shared::MediaItem> items = library.items();
    const shared::MediaItem* work = findItem(items, "Work");
    CHECK(work && work->type == shared::MediaType::Card && work->inUse && !work->isTemplate);
    CHECK(work && work->fileName == "Work.card.yaml");
    if (!work) return;
    const shared::MediaItem workItem = *work;
    std::vector<std::uint8_t> bytes;
    CHECK(library.exportBytes(workItem.path, &bytes) && !bytes.empty());
    CHECK(!library.remove(workItem.path).ok);  // in slot 1

    CHECK(f.app->runNow(f.app->selectModuleJob(1, "")).ok);
    CHECK(library.remove(workItem.path).ok);
    CHECK(library.items().empty());
    CHECK(library.importFile("Work.card.yaml", bytes).ok);
    CHECK(findItem(library.items(), "Work") != nullptr);
    CHECK(!library.importFile("Work.card.yaml", bytes).ok);  // exists
    shared::OpResult r = library.importFile("x.card.yaml", {'n', 'o', 'p', 'e'});
    CHECK(!r.ok && r.error.find("isn't a card file") != std::string::npos);
    // A bundled card's name is taken.
    std::vector<std::uint8_t> bundled = readBytes("Qt6/resources/cards/ce161.card.yaml");
    CHECK(!bundled.empty() && !library.importFile("ce161.card.yaml", bundled).ok);

    const std::vector<std::uint8_t> wav = readBytes("Core/tests/fixtures/tape/pc1500_tape.wav");
    CHECK(library.importFile("Side A.wav", wav).ok);
    CHECK(!library.importFile("side a.wav", wav).ok);
    CHECK(!library.importFile("Broken.wav", {'R', 'I', 'F', 'F'}).ok);
    CHECK(findItem(library.items(), "Broken") == nullptr);
    items = library.items();
    const shared::MediaItem* tape = findItem(items, "Side A");
    CHECK(tape && tape->type == shared::MediaType::Tape && !tape->inUse);
    CHECK(!library.importFile("notes.txt", {'x'}).ok);
}

// Name & Save with "write-protect": the file records the switch, the live
// card stops taking writes at once, and the saved instance comes back
// protected with its contents. A card without a switch is refused.
void test_card_write_protect() {
    Fixture f;
    f.settings.setStartupModelPreference("PC1500");
    f.app->start();
    CHECK(f.app->runNow(f.app->selectModuleJob(1, "CE-161")).ok);
    CHECK(f.app->modules().canNameAndSave(1));
    CHECK(f.app->modules().canWriteProtect(1));
    PC1500Machine* m = f.app->machine().pc1500();
    CHECK(m != nullptr);
    if (!m) return;
    m->memory().writeME0(0x0000, 0x55);
    CHECK(m->memory().readME0(0x0000) == 0x55);

    std::string error;
    CHECK(f.app->modules().nameAndSave(1, "Rom", &error, /*writeProtect=*/true));
    CHECK(!f.app->modules().canWriteProtect(1));
    const ExpansionCard* card = f.app->machine().slotCard(1);
    CHECK(card && card->writeProtected());
    m->memory().writeME0(0x0000, 0xAA);  // the switch blocks it
    CHECK(m->memory().readME0(0x0000) == 0x55);
    std::ifstream in(fs::path(f.paths->instanceDir()) / "Rom.card.yaml");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("write-protect: protected") != std::string::npos);

    // Reloaded from its file: still protected, contents kept.
    CHECK(f.app->runNow(f.app->selectModuleJob(1, "")).ok);
    CHECK(f.app->runNow(f.app->selectModuleJob(1, "Rom")).ok);
    m = f.app->machine().pc1500();
    card = f.app->machine().slotCard(1);
    CHECK(m && card && card->writeProtected());
    if (m) CHECK(m->memory().readME0(0x0000) == 0x55);

    CHECK(f.app->runNow(f.app->selectModuleJob(1, "CE-1638")).ok);  // battery-backed, no switch
    CHECK(!f.app->modules().canWriteProtect(1));
    CHECK(!f.app->modules().nameAndSave(1, "Locked", &error, true));
    CHECK(error == "\"CE-1638\" has no write-protect switch.");
}

// cardYamlDump(): a header and the initial-content block per slot card,
// or one line saying there's nothing.
void test_card_yaml_dump() {
    Fixture f;
    f.settings.setStartupModelPreference("PC1600");
    f.app->start();
    std::vector<std::string> dump = shared::cardYamlDump(*f.app);
    CHECK(dump.size() == 1 && dump[0].find("no card attached") != std::string::npos);
    CHECK(f.app->runNow(f.app->selectModuleJob(2, "CE-1600M")).ok);
    dump = shared::cardYamlDump(*f.app);
    CHECK(dump.size() == 2 && dump[0].find("CE-1600M (Slot 2), unbanked") != std::string::npos);
}

// pickModelJob(): a fresh machine of the model, then its default preset;
// defaultPresetJob() is ok with none set.
void test_pick_model() {
    Fixture f;
    f.settings.setStartupModelPreference("PC1500A");
    f.app->start();
    CHECK(f.app->runNow(f.app->defaultPresetJob()).ok);  // none set
    f.settings.setDefaultPresetPath(Model::PC1600, "Core/tests/fixtures/presets/startup-pc1600.pc1600");
    CHECK(f.app->runNow(f.app->pickModelJob(Model::PC1600)).ok);
    CHECK(f.app->machine().currentModel() == Model::PC1600);
    CHECK(f.app->machine().ce1600pAttached());  // the preset's plotter
    // Re-picking the model does nothing.
    CHECK(f.app->runNow(f.app->togglePeripheralJob(shared::Peripheral::CE1600P)).ok);
    CHECK(f.app->runNow(f.app->pickModelJob(Model::PC1600)).ok);
    CHECK(!f.app->machine().ce1600pAttached());
}

// A bundled default preset by its "examples:" ref, as Settings stores it.
void test_example_ref_default_preset() {
    Fixture f;
    f.settings.setStartupModelPreference("PC1500A");
    f.app->start();
    f.settings.setDefaultPresetPath(Model::PC1500A, "examples:startup/default-pc1500a.pc1500a");
    const OpResult r = f.app->runNow(f.app->defaultPresetJob());
    if (!r.ok) std::fprintf(stderr, "  (%s)\n", r.error.c_str());
    CHECK(r.ok);
    CHECK(f.app->machine().ce150Attached());
    // Another model's default is refused by its name, not its path.
    const OpResult wrong =
        f.app->runNow(f.app->loadDefaultPresetJob("examples:startup/default-pc1600.pc1600", Model::PC1500A));
    CHECK(!wrong.ok && wrong.error.find("\"CE-1600P + modules\"") != std::string::npos);
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
    test_program_choice();
    test_machine_code_load();
    test_media_library();
    test_card_write_protect();
    test_card_yaml_dump();
    test_pick_model();
    test_example_ref_default_preset();
    std::printf("shared_app_session_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
