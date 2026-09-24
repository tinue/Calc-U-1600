#include "PC1500PresetLoader.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "../Resources/BundledRomCatalog.hpp"
#include "PC1500BasicLoader.hpp"
#include "PC1500BasicTyper.hpp"
#include "PC1500Machine.hpp"
#include "PC1500MachineCodeLoader.hpp"
#include "PC1500Screenshot.hpp"

namespace {

constexpr double kCpuHz = PC1500Machine::kCpuHz;
constexpr int kFramesPerSecond = 60;
constexpr uint64_t kCyclesPerFrame = static_cast<uint64_t>(kCpuHz / kFramesPerSecond);

// Generous margin past the ROM's own power-on RAM-check/boot sequence --
// keys sent immediately after reset are missed entirely, since the ROM
// doesn't start polling the keyboard until it settles into its post-boot
// idle loop.
constexpr uint64_t kBootSettleCycles = static_cast<uint64_t>(kCpuHz * 2);
constexpr uint64_t kIdleCap = static_cast<uint64_t>(kCpuHz * 5);

// The ROM's typed-input line buffer (same base/length tools/pc1500_cli.cpp
// reads for its "Display input-line text" dump) -- lets each preset step be
// logged with what actually ended up on the LCD's edit line, so a load
// that goes wrong can be lined up against the calculator's own display.
std::string screenText(PC1500Machine& machine) {
    static constexpr uint16_t kBase = 0x7BB0;
    std::string s;
    for (int i = 0; i < 80; i++) {
        uint8_t b = machine.memory().peek(static_cast<uint16_t>(kBase + i));
        if (b == 0x0D) break;
        s += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
    }
    return s;
}

std::string hex4(uint16_t v) {
    char b[8];
    std::snprintf(b, sizeof(b), "$%04X", v);
    return b;
}

// BREAK (the ON key) isn't part of the physical key matrix -- see
// PC1500Keyboard.hpp's own doc comment -- so it can't go through
// tapKey()/pressKey("break"), which would silently no-op. Scriptable
// equivalent of a plain ON press+release.
void tapBreak(PC1500Machine& machine) {
    machine.setOnKeyPressed(true);
    machine.runCycles(kCyclesPerFrame * 4);
    machine.setOnKeyPressed(false);
    machine.runCycles(kCyclesPerFrame * 4);
}

class PC1500PresetMachine final : public PresetMachineBase<PC1500Machine> {
public:
    using PresetMachineBase::PresetMachineBase;

    uint32_t cyclesPerSecond() const override { return PC1500Machine::kCpuHz; }
    uint64_t waitUntilBasicIdle(uint64_t maxCycles) override {
        return ::waitUntilBasicIdle(m_machine, maxCycles);
    }
    std::string stepTag() override {
        // std::string, not a fixed buffer -- screenText() can return up to 80
        // chars and truncating it would blank exactly the diagnostic (what
        // ended up on the LCD edit line) this tag exists to show.
        return "  screen=\"" + screenText(m_machine) + "\" pc=" + hex4(m_machine.debugPC());
    }

    // The parser already rejected unknown key names.
    bool key(const std::string& name, std::string*) override {
        if (name == "break" || name == "on") tapBreak(m_machine);
        else tapKey(m_machine, name);
        return true;
    }
    bool typeLine(const std::string& line, std::string* error) override {
        return ::typeLine(m_machine, line, /*pressEnter=*/true, error);
    }

    bool writeScreenshot(const std::string& path, std::string* error) override {
        return writeLcdScreenshotPng(pc1500LcdBitmap(m_machine), kPC1500ScreenMm, path, error);
    }

    BasicTypeResult typeBasicProgram(const std::string& text) override {
        return typeBasicProgramText(m_machine, text);
    }
    basic::TransferModel transferModel() const override { return basic::TransferModel::PC1500; }
    BasicLoadResult loadBasicPayload(const std::vector<uint8_t>& payload) override {
        return loadBasicBinaryPayload(m_machine, payload);
    }

    bool loadBinary(const PresetProgram& program, const std::string& tag, const PresetLogFn& log,
                    std::string* error) override {
        errno = 0;
        std::ifstream in(program.path, std::ios::binary);
        if (!in) {
            // errno detail -- "Permission denied" (a sandbox-denied path) and
            // "No such file or directory" (a genuinely missing/mistyped path)
            // look identical from `!in` alone otherwise.
            *error = tag + "failed to open program file: " + program.path + " (" + std::strerror(errno) + ")";
            return false;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string loadError;
        if (!loadPC1500MachineCode(m_machine, program.address, bytes.data(), bytes.size(), &loadError)) {
            *error = tag + "binary " + program.path + ": " + loadError;
            return false;
        }
        if (log)
            log(tag + "binary " + program.path + " (" + std::to_string(bytes.size()) + " bytes) -> " +
                hex4(program.address));
        return true;
    }
};

} // namespace

PresetLoadResult applyPC1500Preset(PC1500Machine& machine, const PresetFile& preset,
                                   const PresetLogFn& log,
                                   const std::string& traceDir, const std::string& moduleDir,
                                   const PresetBootedFn& onBooted, const std::vector<std::string>& romDirs,
                                   const std::vector<std::string>& extraModuleDirs,
                                   const PresetArmedFn& onArmed,
                                   const PresetSaveAsFn& onSaveAs) {
    PresetLoadResult result;
    PC1500PresetMachine adapter(machine);

    if (!BundledRoms::loadPC1500Rom(machine, preset.romVariant, romDirs, &result.error)) {
        if (log) log("ROM load FAILED: " + result.error);
        return result;
    }
    if (log) log("ROM loaded: revision " + preset.romVariant);
    // Attach before reset/boot-settle, not after -- a real module is
    // physically present before power-on, so the ROM's own boot-time
    // memory sizing sees it too.
    if (!preset.memoryExpansionModuleSpecFile.empty() || !preset.memoryExpansionModuleSpecName.empty()) {
        CardHost host = (preset.variant == PC1500Variant::PC1500A) ? CardHost::PC1500A
                                                                   : CardHost::PC1500;
        std::string err, specPath;
        auto card = makePresetModuleCard(preset.memoryExpansionModuleSpecFile,
                                         preset.memoryExpansionModuleSpecName,
                                         presetModuleDirs(moduleDir, extraModuleDirs), host, &specPath, &err);
        if (!card) {
            result.error = "memory-expansion modulespec: " + err;
            if (log) log("modulespec load FAILED: " + err);
            return result;
        }
        machine.attachExpansionCard(std::move(card));
        result.slot1ResolvedPath = specPath;
        if (log) log("software-defined module attached: " + specPath);
    }

    // The CE-150 plotter, attached before reset -- a real peripheral is
    // physically present at power-on, so the boot ROM's peripheral scan
    // recognises it (the same ordering the CE-1600P uses on the PC-1600).
    if (preset.plotter == "ce150") {
        if (!BundledRoms::attachCE150(machine, romDirs, &result.error)) {
            if (log) log(result.error);
            return result;
        }
        result.ce150Attached = true;
        if (log) log("plotter: CE-150 attached");
    }
    if (preset.interfaceName == "ce158") {
        if (!BundledRoms::attachCE158(machine, romDirs, &result.error)) {
            if (log) log(result.error);
            return result;
        }
        result.ce158Attached = true;
        if (log) log("interface: CE-158 attached");
    }

    // Machine is now fully armed (ROM/module/plotter wired) but still
    // powered off -- give the caller a chance to repaint that state before
    // the boot below makes it start running.
    if (onArmed) onArmed(result);

    machine.reset();
    runBootToPrompt(machine);

    // Connecting the CE-150 (like any memory-map change) makes the ROM do
    // a cold memory check on power-up and stop at the "NEW0? :CHECK"
    // prompt, waiting for a key -- exactly what a real PC-1500 does when
    // you plug the CE-150 in and switch on. Answer it with CL to reach the
    // BASIC "> " prompt, the same key a user would press. Retry a few
    // times: the very first tap while the ROM is still finishing the check
    // can be missed.
    for (int attempt = 0; attempt < 5 && screenText(machine).find("NEW0") != std::string::npos;
         ++attempt) {
        tapKey(machine, "cl");
        machine.runCycles(kBootSettleCycles / 4);
        waitIdle(machine, kIdleCap);
    }

    if (log) log("reset + boot settle done" + adapter.stepTag());
    if (onBooted) onBooted();

    runPresetSections(adapter, preset, &result, log, traceDir, onSaveAs);
    return result;
}
