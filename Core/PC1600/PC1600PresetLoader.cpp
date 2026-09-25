#include "PC1600PresetLoader.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "../Connector/FloppyImageFile.hpp"
#include "../Preset/PresetInterface.hpp"
#include "../Resources/BundledRomCatalog.hpp"
#include "PC1600BasicLoader.hpp"
#include "PC1600BasicTyper.hpp"
#include "PC1600Keyboard.hpp"
#include "PC1600Machine.hpp"
#include "PC1600MachineCodeLoader.hpp"
#include "PC1600Screenshot.hpp"

namespace {

constexpr uint64_t kTStateHz = PC1600Machine::kTStateHz;
constexpr uint64_t kFrameTStates = kTStateHz / 60;

// ON (BREAK) hold, and the idle after it -- see presetTapOn().
constexpr uint64_t kHoldTStates = kFrameTStates * 4;

// The console input line, as ASCII, at the PC-1600 work-area buffer
// FBB0H-FBFFH (PC-1600-Work-Area-Map.md; the same window
// pc1600_preset_tests.cpp reads). Logged after each step so a stuck load
// can be lined up against the LCD's edit line.
std::string screenText(PC1600Machine& machine) { return presetInputLine(machine, 0xFBB0); }

std::string stepTag(PC1600Machine& machine) {
    // Whichever CPU currently owns the bus -- the LH5803 (BASIC-compat)
    // runs the line editor, the SC7852 the rest; label which is shown.
    char pc[24];
    if (machine.sc7852Owns())
        std::snprintf(pc, sizeof(pc), " pc=Z:$%04X", machine.sc7852().pc());
    else
        std::snprintf(pc, sizeof(pc), " pc=L:$%04X", machine.lh5803().pc());
    return "  screen=\"" + screenText(machine) + "\"" + pc;
}

// Attach the plotter the preset's `plotter:` asks for, before the cold
// boot below so the boot ROM detects it -- and, if `floppy:` named a saved
// CE-1600F disk, resolve it by disk-name and load it into the union-attached
// CE1600FCard (read and validated before attaching, so a bad file leaves no
// plotter behind). `moduleDirs` is the same bundled-then-save-folder list
// `- modulespec:` resolution searches. Returns false with result->error
// set on any problem.
bool attachPresetPlotter(PC1600Machine& machine, const std::string& plotter,
                         const std::string& ce1600pRom, const std::string& floppy, int floppySide, const std::vector<std::string>& romDirs,
                         const std::vector<std::string>& moduleDirs, const PresetLogFn& log,
                         PresetLoadResult* result) {
    if (plotter.empty()) return true;

    FloppyFile disk;
    if (!floppy.empty()) {
        std::string path, err;
        if (!resolveFloppyByName(moduleDirs, floppy, &path, &err) || !readFloppyFile(path, &disk, &err)) {
            result->error = "floppy: " + err;
            return false;
        }
        result->floppyImageLabel = floppy;
        result->floppyResolvedPath = path;
    }

    if (!BundledRoms::attachPlotterByName(machine, plotter, romDirs, &result->error, &result->ce150Attached,
                                          ce1600pRom)) {
        return false;
    }
    if (!floppy.empty()) {
        machine.ce1600fLoadImage(disk.image.data(), disk.image.size());  // resets to side A
        if (floppySide != 0) machine.ce1600fSetSide(floppySide);
    }
    if (log) {
        log(plotter == "ce150" ? "plotter: CE-150 attached (LH5803 side)"
                                : "plotter: " + plotter + (plotter == "ce1600p" ? ":" + ce1600pRom : "") + " attached" +
                                      (floppy.empty() ? "" : " (floppy: " + floppy + ")"));
    }
    return true;
}

class PC1600PresetMachine final : public PresetMachineBase<PC1600Machine> {
public:
    using PresetMachineBase::PresetMachineBase;

    uint32_t cyclesPerSecond() const override { return kTStateHz; }
    uint64_t waitUntilBasicIdle(uint64_t maxTStates) override {
        return ::waitUntilBasicIdle(m_machine, maxTStates);
    }
    std::string stepTag() override { return ::stepTag(m_machine); }

    // `break`/`on` (the ON key) or a single named PC-1600 key
    // (PC1600Keyboard::keyFromName).
    bool key(const std::string& name, std::string* error) override {
        // Same gap as between typed lines: after a prior line's ENTER the ROM
        // is out of its key-scan loop for a bit. No-op without a plotter.
        waitForKeyboardScanLoop(m_machine);
        if (name == "break" || name == "on") {
            presetTapOn(m_machine, kHoldTStates);
            return true;
        }
        if (PC1600Keyboard::keyFromName(name) == PC1600Keyboard::Key::Unknown) {
            *error = "key: '" + name + "' is not a known PC-1600 key";
            return false;
        }
        tapKey(m_machine, name);
        return true;
    }
    // Case-sensitive; lowercase and the shifted punctuation / digit-row
    // symbols go via a SHIFT-tap (see PC1600BasicTyper::typeLine).
    bool typeLine(const std::string& line, std::string* error) override {
        return ::typeLine(m_machine, line, /*pressEnter=*/true, error);
    }

    // The graphics area (no status strip), same image as the GUI's Copy Screen.
    bool writeScreenshot(const std::string& path, std::string* error) override {
        return writeLcdScreenshotPng(pc1600LcdBitmap(m_machine), kPC1600ScreenMm, path, error);
    }

    BasicTypeResult typeBasicProgram(const std::string& text) override {
        return typeBasicProgramText(m_machine, text);
    }
    basic::TransferModel transferModel() const override { return basic::TransferModel::PC1600; }
    BasicLoadResult loadBasicPayload(const std::vector<uint8_t>& payload) override {
        return loadBasicBinaryPayload(m_machine, payload);
    }

    machinecode::Target codeTarget() const override { return machinecode::Target::PC1600; }
    // Linear, into exactly the one slot the preset names: S0 = internal RAM
    // ($C000-$FFFF), S1/S2 = the $8000-$BFFF memory-slot window. Straight
    // into the backing store, so the current bank state doesn't matter and
    // no ADTBL scatter applies (unlike the BASIC fast loader).
    bool loadMachineCode(const PresetProgram& program, uint32_t addr, const uint8_t* data, size_t len,
                         std::string* error) override {
        return loadPC1600MachineCode(m_machine, static_cast<int>(program.slot), addr, data, len, error);
    }
};

} // namespace

PresetLoadResult applyPC1600Preset(PC1600Machine& machine, const PresetFile& preset,
                                   const PresetLogFn& log, const std::string& traceDir,
                                   const std::string& moduleDir,
                                   const PresetBootedFn& onBooted,
                                   const std::vector<std::string>& romDirs,
                                   const std::vector<std::string>& extraModuleDirs,
                                   const PresetArmedFn& onArmed,
                                   const PresetSaveAsFn& onSaveAs) {
    PresetLoadResult result;
    PC1600PresetMachine adapter(machine);
    const std::vector<std::string> moduleDirs = presetModuleDirs(moduleDir, extraModuleDirs);

    auto plug = [&](const std::string& specFile, const std::string& specName, int slot) -> bool {
        if (specFile.empty() && specName.empty()) return true;  // empty slot
        CardHost host = (slot == 1) ? CardHost::PC1600Slot1 : CardHost::PC1600Slot2;
        std::string err, specPath;
        std::unique_ptr<ExpansionCard> card =
            makePresetModuleCard(specFile, specName, moduleDirs, host, &specPath, &err);
        if (!card) {
            result.error = "slot " + std::to_string(slot) + " modulespec: " + err;
            return false;
        }
        const std::string label = card->moduleName() + " (" + specPath + ")";
        if (slot == 1) {
            machine.attachSlot1Card(std::move(card));
            result.slot1ResolvedPath = specPath;
        } else {
            machine.attachSlot2Card(std::move(card));
            result.slot2ResolvedPath = specPath;
        }
        if (log) log("slot " + std::to_string(slot) + ": " + label + " attached");
        return true;
    };

    // Arm the machine -- modules, then (before the reset below, so the boot
    // ROM's peripheral scan sees them, as on real hardware: power off,
    // connect, power on) the plotter and the CE-158 next to (or instead of)
    // the CE-150; the parser already refused the CE-158 with the CE-1600P.
    // Stops at the first failure.
    const bool armed = plug(preset.slot1ModuleSpecFile, preset.slot1ModuleSpecName, 1) &&
                       plug(preset.slot2ModuleSpecFile, preset.slot2ModuleSpecName, 2) &&
                       attachPresetPlotter(machine, preset.plotter, preset.ce1600pRomVariant, preset.floppy,
                                           preset.floppySide, romDirs, moduleDirs, log, &result) &&
                       attachPresetInterface(machine, preset.interfaceName, romDirs, log, &result);

    // Machine is now armed (model/cards/plotter wired) but still powered
    // off -- give the caller a chance to repaint that state before the
    // boot below makes it start running. Fired on a failed arming too, so
    // the caller always sees what did get attached.
    if (onArmed) onArmed(result);
    if (!armed) return result;

    // Full cold boot: the slot config just changed, so the IOCS work area
    // must be rebuilt from scratch (simple reset() would keep stale RAM).
    machine.allReset();
    // Includes the plotter's power-on init -- `waitForKeyboardScanLoop`
    // also guards every typed line (PC1600BasicTyper) and every `key:` step
    // (PC1600PresetMachine::key) for the same gap that reopens after each line's ENTER.
    runBootToPrompt(machine);
    if (log) log("reset + boot settle done" + adapter.stepTag());
    if (onBooted) onBooted();

    runPresetSections(adapter, preset, &result, log, traceDir, onSaveAs);
    return result;
}
