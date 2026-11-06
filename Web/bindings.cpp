// The web client's adapter to Shared/ (docs/developer/Web-Client.md): one
// shared::AppSession behind an embind surface for calc-engine-worker.js.
// Like the Qt app's MainWindow/MachineController it only translates: input
// and files in, what changed posted back as plain JS objects the page draws
// without further rules -- the LCD dots and indicators, the control state
// and pickers, plotter strokes in pane pixels, printer text.
//
// Single-threaded, cooperative: operations (presets, resets, rebuilds) run
// a 10 ms slice per tick() (AppSession::start()). Files live in Emscripten's
// file system: /bundle (ROMs, bundled cards and disks), /presets (the
// bundled examples), /uploads (files the user opened) and /persist, the
// home folder, kept in IndexedDB by the worker (IDBFS, autoPersist).
//
// Built by Web/build.sh (CMake target calcu_web) into dist/wasm/.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Connector/Ce158Card.hpp"
#include "Debug/Inspect/Inspector.hpp"
#include "Display/LcdText.hpp"
#include "DropFile.hpp"
#include "Shared/AppSession.hpp"
#include "Shared/CardYamlDump.hpp"
#include "Shared/HostKeyMap.hpp"
#include "Shared/KeyLayout/FaceplateLayout.hpp"
#include "Shared/LcdLayout.hpp"
#include "Shared/MachineCodeLoad.hpp"
#include "Shared/MediaLibrary.hpp"
#include "Shared/PlotterPaper.hpp"
#include "Shared/Utf8Path.hpp"

using namespace emscripten;
namespace fs = std::filesystem;

namespace {

constexpr const char* kHomeDir = "/persist";
constexpr const char* kBundleDir = "/bundle";
constexpr const char* kUploadDir = "/uploads";

void post(val message) { val::global("postMessage")(message); }

val obj() { return val::object(); }

template <typename T, typename F>
val array(const std::vector<T>& items, F&& convert) {
    val out = val::array();
    for (std::size_t i = 0; i < items.size(); ++i) out.set(i, convert(items[i]));
    return out;
}

val strings(const std::vector<std::string>& items) {
    return array(items, [](const std::string& s) { return val(s); });
}

val bytesToJS(const std::vector<std::uint8_t>& bytes) {
    val out = val::global("Uint8Array").new_(bytes.size());
    out.call<void>("set", val(typed_memory_view(bytes.size(), bytes.data())));
    return out;
}

std::vector<std::uint8_t> bytesFromJS(val bytes) { return convertJSArrayToNumberVector<std::uint8_t>(bytes); }

val rgbJS(shared::Rgb c) {
    val v = val::array();
    v.set(0, c.r);
    v.set(1, c.g);
    v.set(2, c.b);
    return v;
}

const char* modelName(shared::Model m) { return shared::modelKey(m); }

shared::HostKey hostKeyFromCode(const std::string& code) {
    using K = shared::HostKey;
    static const std::map<std::string, K> keys = {
        {"Backspace", K::Backspace}, {"Enter", K::Return},     {"NumpadEnter", K::Enter}, {"Tab", K::Tab},
        {"Delete", K::Delete},       {"ArrowUp", K::Up},       {"ArrowDown", K::Down},    {"ArrowLeft", K::Left},
        {"ArrowRight", K::Right},    {"F1", K::F1},            {"F2", K::F2},             {"F3", K::F3},
        {"F4", K::F4},               {"F5", K::F5},            {"F6", K::F6},             {"ScrollLock", K::ScrollLock},
        {"Insert", K::Insert},       {"Home", K::Home},        {"End", K::End},           {"PageUp", K::PageUp},
        {"PageDown", K::PageDown},
    };
    const auto it = keys.find(code);
    return it == keys.end() ? K::Other : it->second;
}

// The one code point `utf8` holds, 0 for none or more than one.
char32_t singleCodePoint(const std::string& utf8) {
    if (utf8.empty()) return 0;
    const auto c = static_cast<unsigned char>(utf8[0]);
    int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (len == 0 || static_cast<int>(utf8.size()) != len) return 0;
    char32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
    for (int i = 1; i < len; ++i) cp = (cp << 6) | (static_cast<unsigned char>(utf8[i]) & 0x3F);
    return cp;
}

// Each code point of `utf8` as its own UTF-8 string.
std::vector<std::string> codePoints(const std::string& utf8) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < utf8.size();) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        const std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        out.push_back(utf8.substr(i, len));
        i += len;
    }
    return out;
}

bool writeFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(shared::toPath(path).parent_path(), ec);
    std::ofstream out(shared::toPath(path), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// ── The browser's audio output ───────────────────────────────────────────
// Samples go to the page (an AudioWorklet's ring buffer) as they come; how
// much is still queued there is estimated from what was sent and the time
// since the sink opened.
class WebAudioSink : public shared::AudioSink {
public:
    bool start(int sampleRate, int bufferMs) override {
        m_rate = sampleRate;
        m_capacity = static_cast<std::size_t>(sampleRate) * static_cast<std::size_t>(bufferMs) / 1000 * 2;
        m_sent = 0;
        m_opened = now();
        val m = obj();
        m.set("type", "audio");
        m.set("op", "start");
        m.set("rate", sampleRate);
        post(m);
        return true;
    }
    void stop() override {
        val m = obj();
        m.set("type", "audio");
        m.set("op", "stop");
        post(m);
    }
    std::size_t room() const override {
        const std::size_t queued = queuedSamples();
        return queued >= m_capacity ? 0 : m_capacity - queued;
    }
    void write(const std::int16_t* samples, std::size_t count) override {
        if (count == 0) return;
        // A sink that ran dry restarts its clock with this write.
        if (queuedSamples() == 0) {
            m_opened = now();
            m_sent = 0;
        }
        val pcm = val::global("Int16Array").new_(count);
        pcm.call<void>("set", val(typed_memory_view(count, samples)));
        val m = obj();
        m.set("type", "audio");
        m.set("op", "data");
        m.set("pcm", pcm);
        post(m);
        m_sent += count;
    }
    bool ranDry() const override { return m_sent > 0 && queuedSamples() == 0; }

private:
    static double now() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::size_t queuedSamples() const {
        const double played = (now() - m_opened) * m_rate;
        return played >= static_cast<double>(m_sent) ? 0 : m_sent - static_cast<std::size_t>(played);
    }
    int m_rate = 48000;
    std::size_t m_capacity = 0;
    std::size_t m_sent = 0;
    double m_opened = 0;
};

// ── Settings in the page's localStorage ──────────────────────────────────
// Seeded from what the page stored; every change is posted back.
class WebSettingsStore : public shared::SettingsStore {
public:
    explicit WebSettingsStore(val stored) {
        val keys = val::global("Object").call<val>("keys", stored);
        const int n = keys["length"].as<int>();
        for (int i = 0; i < n; ++i) {
            const std::string key = keys[i].as<std::string>();
            val v = stored[key];
            if (v.isString()) m_values[key] = v.as<std::string>();
        }
    }
    std::optional<std::string> get(const std::string& key) const override {
        const auto it = m_values.find(key);
        if (it == m_values.end()) return std::nullopt;
        return it->second;
    }
    void set(const std::string& key, const std::string& value) override {
        if (get(key) == value) return;
        m_values[key] = value;
        val m = obj();
        m.set("type", "store");
        m.set("key", key);
        m.set("value", value);
        post(m);
    }
    void remove(const std::string& key) override {
        if (!m_values.erase(key)) return;
        val m = obj();
        m.set("type", "store");
        m.set("key", key);
        m.set("value", val::null());
        post(m);
    }

private:
    std::map<std::string, std::string> m_values;
};

// ── The controller ───────────────────────────────────────────────────────

class WebController : public shared::AppSession::Platform {
public:
    explicit WebController(val stored)
        : m_store(stored), m_settings(m_store), m_paths(m_settings, shared::HostDirs{kHomeDir, kBundleDir}) {
        m_app = std::make_unique<shared::AppSession>(m_settings, m_paths, *this);
        m_app->attachAudio(m_audio, [](const std::string& text) { log(text); });
        m_library = std::make_unique<shared::MediaLibrary>(*m_app);
    }

    // After construction, once the worker is listening: the startup model,
    // then (as an operation) its default preset.
    void start() {
        m_app->start();
        postEverything();
        run("Default Preset", m_app->defaultPresetJob());
    }

    // The ~60 Hz frame.
    void tick() {
        m_app->tick();
        postFrame();
    }

    // ── Keys ─────────────────────────────────────────────────────────────
    // A faceplate key (pointer) down / up; ON is its own line.
    void faceKeyDown(const std::string& name) {
        if (name == "on") m_app->machine().setOnKeyPressed(true);
        else m_app->machine().pressKey(name);
    }
    void faceKeyUp(const std::string& name) {
        if (name == "on") m_app->machine().setOnKeyPressed(false);
        else m_app->machine().releaseKey(name);
    }
    void setTurbo(bool on) { m_app->pacer().setTurbo(on); }

    // A host key down (KeyboardEvent.code / .key and modifiers): the
    // calculator key the page must release on the key's keyup, or "" when
    // nothing needs releasing (or the key doesn't type).
    std::string hostKeyDown(const std::string& code, const std::string& key, bool shift, bool control, bool alt,
                            bool meta, bool macHost) {
        shared::HostModifiers mods;
        mods.shift = shift;
        mods.alt = alt;
        if (macHost) {
            mods.command = meta;
            mods.control = control;
        } else {
            mods.control = control;
            mods.meta = meta;
        }
        const auto resolved = shared::resolveHostKey(hostKeyFromCode(code), mods, singleCodePoint(key),
                                                     isPC1600(), macHost);
        if (!resolved) return "\x01";  // not a calculator key: the page lets the browser have it
        return m_app->machine().hostKeyDown(*resolved, /*physical=*/true) ? resolved->baseKey : std::string();
    }
    void hostKeyUp(const std::string& baseKey) { m_app->machine().hostKeyUp(baseKey); }
    void hostShiftTap() { m_app->machine().hostShiftTap(); }
    // Committed text from a composition (dead keys, IME): a tap per character.
    void typeCommitted(const std::string& text) {
        for (const std::string& c : codePoints(text)) {
            const auto resolved =
                shared::resolveHostKey(shared::HostKey::Other, {}, singleCodePoint(c), isPC1600(), false);
            if (resolved) m_app->machine().hostKeyDown(*resolved, /*physical=*/false);
        }
    }
    void pasteText(const std::string& text) { m_app->machine().pasteText(text); }

    // ── Operations ───────────────────────────────────────────────────────
    void pickModel(int model) { run("Machine", m_app->pickModelJob(static_cast<shared::Model>(model))); }
    void selectModule(int slot, const std::string& name) { run("Memory Module", m_app->selectModuleJob(slot, name)); }
    void selectPC1500Rom(int revision) {
        if (static_cast<PC1500RomRevision>(revision) == m_app->machine().pc1500RomRevision()) return;
        run("ROM", m_app->selectPC1500RomJob(static_cast<PC1500RomRevision>(revision)));
    }
    void selectPC1600Rom(int version) {
        if (static_cast<shared::PC1600RomVersion>(version) == m_app->machine().pc1600RomVersion()) return;
        run("ROM", m_app->selectPC1600RomJob(static_cast<shared::PC1600RomVersion>(version)));
    }
    void selectCE1600PRom(int version) {
        if (static_cast<shared::CE1600PRomVersion>(version) == m_app->machine().ce1600pRomVersion()) return;
        run("ROM", m_app->selectCE1600PRomJob(static_cast<shared::CE1600PRomVersion>(version)));
    }
    void togglePeripheral(int p) { run("Peripheral", m_app->togglePeripheralJob(static_cast<shared::Peripheral>(p))); }
    void reset(bool all) { run(all ? "Reset All" : "Reset", m_app->resetJob(all)); }
    // A preset by its path in the file system (/presets/..., /uploads/...).
    void loadPreset(const std::string& path) { run("Load Preset", m_app->loadPresetJob(path)); }

    // ── Opening files ────────────────────────────────────────────────────
    // A file the user picked or dropped goes to /uploads first (a preset
    // finds the files it names next to it); returns its path.
    std::string upload(const std::string& name, val bytes) {
        const std::string path = std::string(kUploadDir) + "/" + shared::fromPath(shared::toPath(name).filename());
        writeFile(path, bytesFromJS(bytes));
        return path;
    }
    // What a dropped file is: "preset", "basic", "code" or "".
    std::string dropTarget(const std::string& name, val bytes) {
        switch (dropfile::classify(bytesFromJS(bytes), name)) {
            case dropfile::Target::Preset: return "preset";
            case dropfile::Target::BasicProgram: return "basic";
            case dropfile::Target::MachineCode: return "code";
            case dropfile::Target::None: break;
        }
        return "";
    }
    // Load BASIC Program / Load Machine Code, step 1: the images in the
    // file. {names: [...]} when the user must pick one (pickProgram()).
    val openProgram(const std::string& name, val bytes) {
        m_program = shared::programChoice(bytesFromJS(bytes));
        m_programName = name;
        val out = obj();
        out.set("names", strings(m_program.names));
        return out;
    }
    // Step 2, BASIC: the picked image (0 when there was no choice) into the
    // running machine. "" or the error.
    std::string loadBasic(int index) {
        if (index < 0 || index >= static_cast<int>(m_program.images.size())) return "No program picked.";
        const shared::OpResult r =
            m_app->loadBasicProgram(m_program.images[static_cast<std::size_t>(index)], m_programName);
        postEverything();
        return r.ok ? std::string() : r.error;
    }
    // Step 2, machine code: the plan -- {error} or {needsAddress, ...}.
    val planMachineCode(int index) {
        val out = obj();
        if (index < 0 || index >= static_cast<int>(m_program.images.size())) {
            out.set("error", "No program picked.");
            return out;
        }
        m_code = shared::MachineCodeLoad::plan(m_app->machine(), m_program.images[static_cast<std::size_t>(index)]);
        out.set("error", m_code->error());
        out.set("needsAddress", m_code->needsAddress());
        out.set("defaultAddress", m_code->defaultAddressText());
        out.set("placeholder", m_code->addressPlaceholder());
        out.set("cpuNote", m_code->cpuNote());
        out.set("length", static_cast<double>(m_code->length()));
        return out;
    }
    // The address dialog's check of what is typed: {valid, hint}.
    val checkAddress(const std::string& text) {
        val out = obj();
        if (!m_code) return out;
        const auto check = m_code->setAddress(text);
        out.set("valid", check.valid);
        out.set("hint", check.hint);
        return out;
    }
    // Step 3: load it. {error} or {summary, newCommand, newNote,
    // callCommand, callNote}; the page shows it, then types the CALL.
    val loadMachineCode() {
        val out = obj();
        if (!m_code) return out;
        const shared::OpResult r = m_app->loadMachineCode(m_code->request());
        if (!r.ok) {
            out.set("error", r.error);
            return out;
        }
        const auto loaded = m_code->loaded(m_app->machine());
        out.set("summary", loaded.summary);
        out.set("newCommand", loaded.advice.newCommand);
        out.set("newNote", loaded.advice.newNote);
        out.set("callCommand", loaded.advice.callCommand);
        out.set("callNote", loaded.advice.callNote);
        postEverything();
        return out;
    }

    // ── Bays ─────────────────────────────────────────────────────────────
    // Name & Save / tape errors come back as "" or the sentence to show.
    std::string nameAndSaveCard(int slot, const std::string& name) {
        std::string error;
        m_app->modules().nameAndSave(slot, name, &error);
        postEverything();
        return error;
    }
    void selectDisk(const std::string& name) {
        m_app->floppy().selectDisk(name);
        postEverything();
    }
    void toggleDiskSide() {
        m_app->floppy().toggleSide();
        postEverything();
    }
    std::string nameAndSaveDisk(const std::string& name) {
        std::string error;
        m_app->floppy().nameAndSave(name, &error);
        postEverything();
        return error;
    }
    std::string selectTape(const std::string& name) {
        std::string error;
        m_app->tapes().selectForPlay(name, &error);
        postEverything();
        return error;
    }
    bool tapeExists(const std::string& name) const { return m_app->tapes().exists(name); }
    std::string recordTape(const std::string& name) {
        const bool ok = m_app->tapes().recordNew(name);
        postEverything();
        return ok ? std::string() : std::string("A tape needs a name.");
    }

    // ── Saved media ──────────────────────────────────────────────────────
    val mediaItems() const {
        return array(m_library->items(), [](const shared::MediaItem& item) {
            val v = obj();
            v.set("type", item.type == shared::MediaType::Card ? "card"
                          : item.type == shared::MediaType::Disk ? "disk"
                                                                 : "tape");
            v.set("name", item.name);
            v.set("fileName", item.fileName);
            v.set("path", item.path);
            v.set("isTemplate", item.isTemplate);
            v.set("inUse", item.inUse);
            return v;
        });
    }
    val exportMedia(const std::string& path) const {
        for (const auto& item : m_library->items()) {
            if (item.path != path) continue;
            std::vector<std::uint8_t> bytes;
            if (m_library->exportBytes(item, &bytes)) return bytesToJS(bytes);
        }
        return val::null();
    }
    std::string importMedia(const std::string& name, val bytes) {
        const shared::OpResult r = m_library->importFile(name, bytesFromJS(bytes));
        postEverything();
        return r.ok ? std::string() : r.error;
    }
    std::string removeMedia(const std::string& path) {
        for (const auto& item : m_library->items()) {
            if (item.path != path) continue;
            const shared::OpResult r = m_library->remove(item);
            postEverything();
            return r.ok ? std::string() : r.error;
        }
        return "It is no longer there.";
    }
    static bool importable(const std::string& name) { return shared::MediaLibrary::importable(name); }

    // ── Inspector ────────────────────────────────────────────────────────
    val inspectorMenu(bool dumps) const {
        const bool pc1500 = !isPC1600();
        return array(m_app->machine().inspectorMenu(dumps), [pc1500](const inspect::MenuEntry& e) {
            val v = obj();
            v.set("view", static_cast<int>(e.view));
            v.set("title", std::string(inspect::viewTitle(e.view, pc1500)));
            v.set("enabled", e.enabled);
            return v;
        });
    }
    std::string inspectorView(int view) const {
        return joinLines(m_app->machine().inspectorView(static_cast<inspect::View>(view)));
    }
    val cardYaml() { return strings(shared::cardYamlDump(*m_app)); }

    // ── Copy Screen ──────────────────────────────────────────────────────
    // {width, height, pixels (grey, row-major), dpi, text}.
    val screenImage() const {
        const GrayImage image = m_app->machine().currentScreenImage();
        val out = obj();
        out.set("width", image.width);
        out.set("height", image.height);
        out.set("dpi", image.dpi);
        out.set("pixels", bytesToJS(image.pixels));
        LcdText text;
        out.set("text", m_app->machine().lcdText(&text) ? text.plainText() : std::string());
        return out;
    }
    // The screen as escaped text (tests): LcdText::text().
    std::string lcdText() const {
        LcdText text;
        return m_app->machine().lcdText(&text) ? text.text() : std::string();
    }

    // ── Plotter paper ────────────────────────────────────────────────────
    // The pane width in CSS pixels the page draws the paper at; the strokes
    // come back in it (postPaper()).
    void setPaperWidth(double width) {
        if (width == m_paperWidth) return;
        m_paperWidth = width;
        m_paperRevision = UINT64_MAX;
        postPaper();
    }
    // The physical-size export (Copy / Cut): {width, height, scale,
    // paneWidth, contentHeight, dotsPerMeter, strokes}.
    val paperExport() const {
        const auto points = plotPoints();
        if (points.empty()) return val::null();
        const shared::PaperGeometry geom = paperGeometry();
        const auto [lower, upper] = shared::penYRange(points);
        const shared::PaperExportSize size = shared::paperExportSize(geom, lower, upper);
        val out = obj();
        out.set("width", size.widthPx);
        out.set("height", size.heightPx);
        out.set("scale", size.scale);
        out.set("paneWidth", size.paneWidthPt);
        out.set("contentHeight", size.contentHeightPt);
        out.set("dotsPerMeter", size.dotsPerMeter);
        out.set("strokes", strokesJS(shared::buildPaperStrokes(geom, size.paneWidthPt, points, lower)));
        return out;
    }
    void clearPaper() {
        const shared::ControlState s = m_app->controlState();
        if (s.paper == shared::PaperKind::CE150) m_app->machine().clearCE150Paper();
        else if (s.paper == shared::PaperKind::CE1600P) m_app->machine().clearCE1600PPaper();
        m_paperRevision = UINT64_MAX;
        postPaper();
    }

    // ── Settings ─────────────────────────────────────────────────────────
    val settingsState() const {
        val out = obj();
        out.set("startupModel", m_settings.startupModelPreference());
        val presets = obj();
        for (shared::Model m : {shared::Model::PC1500, shared::Model::PC1500A, shared::Model::PC1600})
            presets.set(modelName(m), m_settings.defaultPresetPath(m));
        out.set("defaultPresets", presets);
        return out;
    }
    void setStartupModel(const std::string& pref) { m_settings.setStartupModelPreference(pref); }
    void setDefaultPreset(int model, const std::string& path) {
        m_settings.setDefaultPresetPath(static_cast<shared::Model>(model), path);
    }

    // ── Static data ──────────────────────────────────────────────────────
    // A model's faceplate: source-art size, key rects and the LCD rect (art
    // pixels), and the LCD's colours and layout constants.
    static val faceplate(int model) {
        const KeyLayout::Layout& layout = KeyLayout::layoutFor(static_cast<shared::Model>(model));
        val out = obj();
        out.set("width", layout.imageWidth);
        out.set("height", layout.imageHeight);
        auto rect = [](const KeyLayout::KeyRect& r) {
            val v = obj();
            v.set("name", std::string(r.name ? r.name : ""));
            v.set("x", r.x);
            v.set("y", r.y);
            v.set("w", r.w);
            v.set("h", r.h);
            return v;
        };
        out.set("lcd", rect(layout.lcdRect));
        val keys = val::array();
        for (std::size_t i = 0; i < layout.keyCount; ++i) keys.set(i, rect(layout.keys[i]));
        out.set("keys", keys);
        return out;
    }
    static val lcdConstants() {
        using namespace shared::lcd;
        val out = obj();
        out.set("p15Background", rgbJS(kP15BackgroundRgb));
        out.set("p16Background", rgbJS(kP16BackgroundRgb));
        out.set("ink", rgbJS(kInkRgb));
        out.set("p15IndicatorBarFraction", kP15IndicatorBarFraction);
        out.set("p15IndicatorFontHeightFraction", kP15IndicatorFontHeightFraction);
        out.set("p15IndicatorTopInsetFraction", kP15IndicatorTopInsetFraction);
        out.set("p15ContentScale", kP15ContentScale);
        out.set("p15MatrixTopGapFraction", kP15MatrixTopGapFraction);
        out.set("p16IndicatorRowTop", kP16IndicatorRowTop);
        out.set("p16LabelFontHeightFraction", kP16LabelFontHeightFraction);
        out.set("p16RowHeightFraction", kP16RowHeightFraction);
        out.set("p16MatrixLeft", kP16MatrixLeft);
        out.set("p16MatrixRight", kP16MatrixRight);
        out.set("p16MatrixTop", kP16MatrixTop);
        out.set("p16MatrixBottom", kP16MatrixBottom);
        val pens = val::array();
        for (int i = 0; i < shared::kPenCount; ++i) pens.set(i, rgbJS(shared::kPenColors[static_cast<std::size_t>(i)]));
        out.set("penColors", pens);
        out.set("penWidth", shared::kPenWidthPt);
        return out;
    }

    // ── AppSession::Platform ─────────────────────────────────────────────
    void romMissing(const std::string& error, const std::string& searchedIn) override {
        val m = obj();
        m.set("type", "fatal");
        m.set("message", error + " (searched " + searchedIn + ")");
        post(m);
    }
    void notice(shared::MachineSession::Notice n, const std::string& detail) override {
        using Notice = shared::MachineSession::Notice;
        switch (n) {
            case Notice::OldPC1600RomUnavailable:
                tell("ROM", "The old PC-1600 ROM set could not be loaded; the new one is used. " + detail);
                break;
            case Notice::OldCE1600PRomUnavailable:
                tell("ROM", "The old CE-1600P ROM could not be loaded; the new one is used. " + detail);
                break;
            case Notice::TapeSaveFailed: tell("Tape", "The tape recording could not be saved. " + detail); break;
        }
    }
    void moduleChanged(int) override { m_dirty = true; }
    void tapesChanged() override { m_dirty = true; }
    void bayError(shared::Bay bay, const std::string& text) override {
        tell(bay == shared::Bay::Modules ? "Memory Module" : bay == shared::Bay::Floppy ? "Floppy Disk" : "Tape", text);
    }
    void presetArmed() override { postEverything(); }
    void presetLog(const std::string& line) override { log(line); }
    void attachFailed(const std::string& device, const std::string& reason) override {
        tell("Attach " + device, reason.empty() ? "The " + device + " could not be attached."
                                                : "The " + device + " could not be attached:\n\n" + reason);
    }
    void busyChanged(bool busy) override {
        val m = obj();
        m.set("type", "busy");
        m.set("busy", busy);
        post(m);
        if (!busy) postEverything();
    }
    bool hostDirectories() const override { return false; }

private:
    bool isPC1600() const { return m_app->machine().currentModel() == shared::Model::PC1600; }

    static std::string joinLines(const std::vector<std::string>& lines) {
        std::string out;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i) out += '\n';
            out += lines[i];
        }
        return out;
    }

    static void log(const std::string& text) {
        val m = obj();
        m.set("type", "log");
        m.set("text", text);
        post(m);
    }

    // A message box for the user.
    static void tell(const std::string& title, const std::string& text) {
        val m = obj();
        m.set("type", "notice");
        m.set("title", title);
        m.set("text", text);
        post(m);
    }

    // Starts an operation; its result comes back as a notice on failure.
    void run(const std::string& title, job::Task<shared::OpResult> op) {
        const bool started = m_app->start(std::move(op), [title](const shared::OpResult& r) {
            if (!r.ok) tell(title, r.error);
        });
        if (!started) tell(title, "Wait until the current operation has finished.");
    }

    // ── Posting ──────────────────────────────────────────────────────────
    // Everything the controls show, after anything may have changed it.
    void postEverything() {
        m_dirty = false;
        m_lastFrameKey.clear();
        m_lastLamps.clear();
        m_paperRevision = UINT64_MAX;
        postUi();
        postFrame();
    }

    // Once per tick: what changes while the machine runs.
    void postFrame() {
        if (m_dirty) {
            m_dirty = false;
            postUi();
        }
        postDisplay();
        postLamps();
        postPaper();
        postPrinter();
    }

    void postUi() {
        const shared::ControlState s = m_app->controlState();
        val ui = obj();
        ui.set("type", "ui");
        ui.set("model", static_cast<int>(s.model));
        ui.set("slotCount", s.slotCount);
        auto peripheral = [](const shared::PeripheralState& p) {
            val v = obj();
            v.set("offered", p.offered);
            v.set("onControlBar", p.onControlBar);
            v.set("attached", p.attached);
            v.set("enabled", p.enabled);
            return v;
        };
        ui.set("ce150", peripheral(s.ce150));
        ui.set("ce1600p", peripheral(s.ce1600p));
        ui.set("ce158", peripheral(s.ce158));
        ui.set("floppyShown", s.floppyShown);
        ui.set("floppyEnabled", s.floppyEnabled);
        ui.set("tapeShown", s.tapeShown);
        ui.set("pc1500RomMenu", s.pc1500RomMenu);
        ui.set("pc1600RomMenu", s.pc1600RomMenu);
        ui.set("ce1600pRomMenu", s.ce1600pRomMenu);
        ui.set("paper", s.paper == shared::PaperKind::CE150     ? "CE-150"
                        : s.paper == shared::PaperKind::CE1600P ? "CE-1600P"
                                                                : "");
        ui.set("printerShown", s.printerShown);
        ui.set("pc1500Rom", static_cast<int>(m_app->machine().pc1500RomRevision()));
        ui.set("pc1600Rom", static_cast<int>(m_app->machine().pc1600RomVersion()));
        ui.set("ce1600pRom", static_cast<int>(m_app->machine().ce1600pRomVersion()));

        // The slot pickers: RAM templates, saved instances, ROM modules.
        val slots = val::array();
        for (int slot = 1; slot <= s.slotCount; ++slot) {
            const auto lists = m_app->modules().moduleLists(shared::ModuleBay::hostForModel(slot, s.model));
            std::vector<std::string> ram, roms, saved;
            for (const auto& e : lists.templates) (e.rom ? roms : ram).push_back(e.moduleName);
            for (const auto& e : lists.instances) saved.push_back(e.moduleName);
            val v = obj();
            v.set("templates", strings(ram));
            v.set("instances", strings(saved));
            v.set("roms", strings(roms));
            v.set("selected", m_app->modules().selectedModuleName(slot));
            v.set("canSave", m_app->modules().canNameAndSave(slot));
            slots.set(slot - 1, v);
        }
        ui.set("slots", slots);

        if (s.floppyShown) {
            const auto disks = m_app->floppy().diskLists();
            val v = obj();
            v.set("templates", strings(disks.templates));
            v.set("instances", strings(disks.instances));
            v.set("selected", m_app->floppy().selectedDiskName());
            v.set("side", m_app->floppy().side());
            v.set("canSave", m_app->floppy().canNameAndSave());
            ui.set("floppy", v);
        }
        if (s.tapeShown) {
            std::vector<std::string> names = m_app->tapes().tapeNames();
            const std::string current = m_app->tapes().currentName();
            if (!current.empty() && std::find(names.begin(), names.end(), current) == names.end())
                names.push_back(current);
            val v = obj();
            v.set("names", strings(names));
            v.set("current", current);
            ui.set("tape", v);
        }
        post(ui);
    }

    void postDisplay() {
        const shared::DisplayFrame frame = m_app->machine().currentDisplay();
        // The frame as a string: a cheap "did anything change" key.
        std::string key;
        key.reserve(frame.pixels.size() + 64);
        key += static_cast<char>(m_app->machine().currentModel());
        key += frame.poweredOn ? 'P' : 'p';
        key += std::to_string(frame.cols) + "x" + std::to_string(frame.rows);
        for (bool b : frame.pixels) key += b ? '1' : '0';
        for (const auto& [name, on] : frame.statusSymbols) key += on ? '1' : '0';
        if (key == m_lastFrameKey) return;
        m_lastFrameKey = std::move(key);

        val m = obj();
        m.set("type", "display");
        m.set("model", static_cast<int>(m_app->machine().currentModel()));
        m.set("cols", frame.cols);
        m.set("rows", frame.rows);
        m.set("poweredOn", frame.poweredOn);
        std::vector<std::uint8_t> dots(frame.pixels.size());
        for (std::size_t i = 0; i < dots.size(); ++i) dots[i] = frame.pixels[i] ? 1 : 0;
        m.set("dots", bytesToJS(dots));
        m.set("indicators",
              array(shared::lcd::indicators(m_app->machine().currentModel(), frame), [](const shared::lcd::Indicator& ind) {
                  val v = obj();
                  v.set("text", ind.text);
                  v.set("x", ind.x);
                  v.set("scale", ind.scale);
                  v.set("style", ind.style == shared::lcd::Indicator::Style::Inverted ? "inverted"
                                 : ind.style == shared::lcd::Indicator::Style::Dot    ? "dot"
                                                                                      : "text");
                  return v;
              }));
        post(m);
    }

    // The floppy lamp and the tape counter.
    void postLamps() {
        const TapeDeck::Status& tape = m_app->tapeStatus();
        const bool floppyMotor = m_app->controlState().floppyShown && m_app->floppy().motorOn();
        const std::string key = std::to_string(floppyMotor) + std::to_string(static_cast<int>(tape.mode)) +
                                std::to_string(tape.motor) + std::to_string(static_cast<int>(tape.position)) +
                                tape.path;
        if (key == m_lastLamps) return;
        m_lastLamps = key;
        val m = obj();
        m.set("type", "lamps");
        m.set("floppyMotor", floppyMotor);
        m.set("tapeMode", tape.mode == TapeDeck::Mode::Play     ? "play"
                          : tape.mode == TapeDeck::Mode::Record ? "record"
                                                                : "");
        m.set("tapeMotor", tape.motor);
        m.set("tapeSeconds", static_cast<int>(tape.position));
        m.set("tapeFile", shared::fromPath(shared::toPath(tape.path).filename()));
        post(m);
    }

    shared::PaperGeometry paperGeometry() const {
        return m_app->controlState().paper == shared::PaperKind::CE150 ? shared::PaperGeometry::ce150()
                                                                        : shared::PaperGeometry::ce1600p();
    }
    std::vector<AlpsPlotterMechanism::FlatPoint> plotPoints() const {
        const shared::PaperKind kind = m_app->controlState().paper;
        if (kind == shared::PaperKind::CE150) return m_app->machine().ce150PlotPoints();
        if (kind == shared::PaperKind::CE1600P) return m_app->machine().ce1600pPlotPoints();
        return {};
    }

    // Per pen: {coords: Float32Array of x,y pairs, starts: polyline starts
    // (pair index), one past the last}.
    static val strokesJS(const shared::PaperStrokes& strokes) {
        val pens = val::array();
        for (int i = 0; i < shared::kPenCount; ++i) {
            std::vector<float> coords;
            std::vector<std::uint32_t> starts;
            for (const shared::Polyline& line : strokes[static_cast<std::size_t>(i)]) {
                starts.push_back(static_cast<std::uint32_t>(coords.size() / 2));
                for (const shared::PaperPoint& p : line) {
                    coords.push_back(static_cast<float>(p.x));
                    coords.push_back(static_cast<float>(p.y));
                }
            }
            starts.push_back(static_cast<std::uint32_t>(coords.size() / 2));
            val pen = obj();
            val c = val::global("Float32Array").new_(coords.size());
            c.call<void>("set", val(typed_memory_view(coords.size(), coords.data())));
            val s = val::global("Uint32Array").new_(starts.size());
            s.call<void>("set", val(typed_memory_view(starts.size(), starts.data())));
            pen.set("coords", c);
            pen.set("starts", s);
            pens.set(i, pen);
        }
        return pens;
    }

    // The strokes at the page's pane width, when the plot changed.
    void postPaper() {
        const shared::PaperKind kind = m_app->controlState().paper;
        if (kind == shared::PaperKind::None || m_paperWidth <= 0) return;
        const std::uint64_t rev = kind == shared::PaperKind::CE150 ? m_app->machine().ce150PlotRevision()
                                                                   : m_app->machine().ce1600pPlotRevision();
        const int kindKey = static_cast<int>(kind);
        if (rev == m_paperRevision && kindKey == m_paperKind) return;
        m_paperRevision = rev;
        m_paperKind = kindKey;
        const auto points = plotPoints();
        const shared::PaperGeometry geom = paperGeometry();
        const auto [lower, upper] = shared::penYRange(points);
        val m = obj();
        m.set("type", "paper");
        m.set("kind", kind == shared::PaperKind::CE150 ? "CE-150" : "CE-1600P");
        m.set("empty", points.empty());
        m.set("contentHeight", geom.contentHeight(0, lower, upper, m_paperWidth));
        m.set("strokes", strokesJS(shared::buildPaperStrokes(geom, m_paperWidth, points, lower)));
        post(m);
    }

    void postPrinter() {
        if (!m_app->machine().ce158Attached()) return;
        const std::vector<std::uint8_t> bytes = m_app->machine().drainCE158PrinterOutput();
        if (bytes.empty()) return;
        val m = obj();
        m.set("type", "printer");
        m.set("bytes", bytesToJS(bytes));
        m.set("text", ce158PrintableText(bytes.data(), bytes.size()));
        post(m);
    }

    WebSettingsStore m_store;
    shared::Settings m_settings;
    shared::AppPaths m_paths;
    WebAudioSink m_audio;
    std::unique_ptr<shared::AppSession> m_app;
    std::unique_ptr<shared::MediaLibrary> m_library;

    shared::ProgramChoice m_program;
    std::string m_programName;
    std::optional<shared::MachineCodeLoad> m_code;

    bool m_dirty = false;
    std::string m_lastFrameKey;
    std::string m_lastLamps;
    double m_paperWidth = 0;
    std::uint64_t m_paperRevision = UINT64_MAX;
    int m_paperKind = -1;
};

} // namespace

EMSCRIPTEN_BINDINGS(calcu1600) {
    class_<WebController>("WebController")
        .constructor<val>()
        .function("start", &WebController::start)
        .function("tick", &WebController::tick)
        .function("faceKeyDown", &WebController::faceKeyDown)
        .function("faceKeyUp", &WebController::faceKeyUp)
        .function("setTurbo", &WebController::setTurbo)
        .function("hostKeyDown", &WebController::hostKeyDown)
        .function("hostKeyUp", &WebController::hostKeyUp)
        .function("hostShiftTap", &WebController::hostShiftTap)
        .function("typeCommitted", &WebController::typeCommitted)
        .function("pasteText", &WebController::pasteText)
        .function("pickModel", &WebController::pickModel)
        .function("selectModule", &WebController::selectModule)
        .function("selectPC1500Rom", &WebController::selectPC1500Rom)
        .function("selectPC1600Rom", &WebController::selectPC1600Rom)
        .function("selectCE1600PRom", &WebController::selectCE1600PRom)
        .function("togglePeripheral", &WebController::togglePeripheral)
        .function("reset", &WebController::reset)
        .function("loadPreset", &WebController::loadPreset)
        .function("upload", &WebController::upload)
        .function("dropTarget", &WebController::dropTarget)
        .function("openProgram", &WebController::openProgram)
        .function("loadBasic", &WebController::loadBasic)
        .function("planMachineCode", &WebController::planMachineCode)
        .function("checkAddress", &WebController::checkAddress)
        .function("loadMachineCode", &WebController::loadMachineCode)
        .function("nameAndSaveCard", &WebController::nameAndSaveCard)
        .function("selectDisk", &WebController::selectDisk)
        .function("toggleDiskSide", &WebController::toggleDiskSide)
        .function("nameAndSaveDisk", &WebController::nameAndSaveDisk)
        .function("selectTape", &WebController::selectTape)
        .function("tapeExists", &WebController::tapeExists)
        .function("recordTape", &WebController::recordTape)
        .function("mediaItems", &WebController::mediaItems)
        .function("exportMedia", &WebController::exportMedia)
        .function("importMedia", &WebController::importMedia)
        .function("removeMedia", &WebController::removeMedia)
        .function("inspectorMenu", &WebController::inspectorMenu)
        .function("inspectorView", &WebController::inspectorView)
        .function("cardYaml", &WebController::cardYaml)
        .function("screenImage", &WebController::screenImage)
        .function("lcdText", &WebController::lcdText)
        .function("setPaperWidth", &WebController::setPaperWidth)
        .function("paperExport", &WebController::paperExport)
        .function("clearPaper", &WebController::clearPaper)
        .function("settingsState", &WebController::settingsState)
        .function("setStartupModel", &WebController::setStartupModel)
        .function("setDefaultPreset", &WebController::setDefaultPreset)
        .class_function("faceplate", &WebController::faceplate)
        .class_function("lcdConstants", &WebController::lcdConstants)
        .class_function("importable", &WebController::importable);
}
