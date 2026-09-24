#include "PC1600PresetLoader.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../Connector/FloppyImageFile.hpp"
#include "../Resources/BundledRomCatalog.hpp"
#include "PC1600BasicLoader.hpp"
#include "PC1600BasicTyper.hpp"
#include "PC1600Keyboard.hpp"
#include "PC1600Machine.hpp"
#include "PC1600MachineCodeLoader.hpp"
#include "PC1600MachineImage.hpp"
#include "PC1600Screenshot.hpp"

namespace {

constexpr uint64_t kTStateHz = PC1600Machine::kTStateHz;
constexpr uint64_t kFrameTStates = kTStateHz / 60;

// ON (BREAK) hold/idle -- tapKey() (PC1600BasicTyper) drives the key
// matrix, but ON isn't a matrix key, so this stays local.
constexpr uint64_t kHoldTStates = kFrameTStates * 4;
constexpr uint64_t kIdleTStates = kFrameTStates * 4;

// The console input line, as ASCII, at the PC-1600 work-area buffer
// FBB0H-FBFFH (PC-1600-Work-Area-Map.md; the same window
// pc1600_preset_tests.cpp reads). Logged after each step so a stuck load
// can be lined up against the LCD's edit line.
std::string screenText(PC1600Machine& machine) {
    std::string s;
    for (uint16_t a = 0xFBB0; a <= 0xFBFF; ++a) {
        uint8_t b = machine.memory().peek(a);
        if (b == 0x0D) break;
        s += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
    }
    return s;
}

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

void tapBreak(PC1600Machine& machine) {
    machine.setOnKeyPressed(true);
    machine.runCycles(kHoldTStates);
    machine.setOnKeyPressed(false);
    machine.runCycles(kIdleTStates);
}

// `errnoOut`, when given, receives the errno from a failed `fopen` --
// EPERM/EACCES (a sandbox-denied path, e.g. a preset's sibling binary the
// file picker never granted access to) look identical to ENOENT (a
// genuinely missing/mistyped path) from the bool return alone, and the
// caller needs to tell them apart to know whether a folder-access retry
// could help.
bool readWholeFile(const std::string& path, std::vector<uint8_t>* out, int* errnoOut = nullptr) {
    errno = 0;
    std::FILE* fh = std::fopen(path.c_str(), "rb");
    if (!fh) {
        if (errnoOut) *errnoOut = errno;
        return false;
    }
    std::fseek(fh, 0, SEEK_END);
    long size = std::ftell(fh);
    std::fseek(fh, 0, SEEK_SET);
    if (size < 0) { std::fclose(fh); return false; }
    out->resize(static_cast<size_t>(size));
    size_t got = out->empty() ? 0 : std::fread(out->data(), 1, out->size(), fh);
    std::fclose(fh);
    return got == out->size();
}

// ── `format: binary` -- load a PC-1600 machine-language block ──────────
//
// Linear load into exactly one slot: S0 = internal RAM ($C000-$FFFF),
// S1/S2 = the $8000-$BFFF memory-slot window. Load address and length come
// from a 16-byte PC-1600 machine-language header (magic FF 10 00 00, type
// 0x10 -- Core/PC1600/PC1600MachineImage.hpp) when the file carries one,
// and each is overridden by `address:` / `length:` in the preset. A file
// with no such header must supply both. When the header's auto-run address
// is non-zero the loader types `CALL &<addr>` afterwards (the machine must
// already be in RUN mode -- true by default after the cold boot) and waits
// for the interpreter to return to its command loop.
//
// Unlike the BASIC fast loader this needs no ADTBL scatter -- the bytes go
// straight into one backing store via debugWriteInternalRam /
// debugWriteSlotImage, which take Z-80 (SC7852) offsets with no +$8000
// conversion.
bool loadMachineBinary(PC1600Machine& machine, const PresetProgram& program, const std::string& tag,
                       const PresetLogFn& log, std::string* error) {

    std::vector<uint8_t> file;
    int readErrno = 0;
    if (!readWholeFile(program.path, &file, &readErrno)) {
        *error = tag + "could not read program file: " + program.path + " (" + std::strerror(readErrno) + ")";
        return false;
    }

    pc1600::MachineImage hdr = pc1600::parsePC1600MachineImage(file);
    if (hdr.hasHeader && !hdr.ok) {
        *error = tag + hdr.error;
        return false;
    }

    const uint8_t* payload = file.data();
    size_t avail = file.size();
    uint32_t loadAddr = program.address;
    uint32_t length = program.length;
    uint32_t autorunAddr = 0;

    if (hdr.hasHeader) {
        payload = file.data() + hdr.headerSize;
        avail = file.size() - hdr.headerSize;
        if (!program.hasAddress) loadAddr = hdr.loadAddr;
        if (!program.hasLength) {
            length = hdr.headerPayloadLen;
            if (hdr.headerPayloadLen != avail) {
                char b[256];
                std::snprintf(b, sizeof(b),
                              "%sPC-1600 machine-language header says %u payload bytes but %zu "
                              "follow the 16-byte header -- add an explicit 'length:' to override",
                              tag.c_str(), hdr.headerPayloadLen, avail);
                *error = b;
                return false;
            }
        }
        autorunAddr = hdr.autorunAddr;
    } else if (!program.hasAddress || !program.hasLength) {
        *error = tag + program.path +
                 " has no PC-1600 machine-language header: both 'address' and 'length' are required";
        return false;
    }

    if (length == 0) {
        *error = tag + "program is empty (length 0)";
        return false;
    }
    if (length > avail) {
        char b[192];
        std::snprintf(b, sizeof(b), "%s'length' %u exceeds the %zu program bytes available in %s",
                      tag.c_str(), length, avail, program.path.c_str());
        *error = b;
        return false;
    }

    const int slot = program.slot == PresetProgram::Slot::S1   ? 1
                     : program.slot == PresetProgram::Slot::S2 ? 2
                                                               : 0;
    const char* slotName = slot == 1 ? "S1" : slot == 2 ? "S2" : "S0";
    std::string writeError;
    if (!loadPC1600MachineCode(machine, slot, loadAddr, payload, length, &writeError)) {
        *error = tag + writeError;
        return false;
    }

    if (log) {
        char b[176];
        std::snprintf(b, sizeof(b), "%sprogram (binary, slot %s, $%04X..$%04X%s)", tag.c_str(),
                      slotName, loadAddr, static_cast<uint32_t>(loadAddr + length - 1),
                      hdr.hasHeader ? ", header" : "");
        log(std::string(b) + stepTag(machine));
    }

    if (autorunAddr != 0) {
        if (autorunAddr > 0xFFFF) {
            char b[208];
            std::snprintf(b, sizeof(b),
                          "%sheader auto-run address $%X is outside bank 0 -- add an explicit "
                          "'- type: CALL #<bank>,&<addr>' step instead",
                          tag.c_str(), autorunAddr);
            *error = b;
            return false;
        }
        char line[24];
        std::snprintf(line, sizeof(line), "CALL &%X", autorunAddr);
        std::string typeErr;
        if (!typeLine(machine, line, /*pressEnter=*/true, &typeErr)) {
            *error = tag + "auto-run '" + line + "' failed: " + typeErr;
            return false;
        }
        constexpr uint64_t kAutorunIdleCap = static_cast<uint64_t>(kTStateHz) * 3600;  // 1 h emulated
        waitUntilBasicIdle(machine, kAutorunIdleCap);
        if (log) log("  auto-run " + std::string(line) + stepTag(machine));
    }
    return true;
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
            tapBreak(m_machine);
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

    bool loadBinary(const PresetProgram& program, const std::string& tag, const PresetLogFn& log,
                    std::string* error) override {
        return loadMachineBinary(m_machine, program, tag, log, error);
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

    if (!plug(preset.slot1ModuleSpecFile, preset.slot1ModuleSpecName, 1))
        return result;
    if (!plug(preset.slot2ModuleSpecFile, preset.slot2ModuleSpecName, 2))
        return result;

    // Plotter (`plotter:`) -- attach before the reset below, so the boot
    // ROM's peripheral scan sees it (mirrors real hardware: power off,
    // connect, power on).
    if (!attachPresetPlotter(machine, preset.plotter, preset.ce1600pRomVariant, preset.floppy,
                             preset.floppySide, romDirs, moduleDirs, log, &result))
        return result;
    // The CE-158 next to (or instead of) the CE-150 -- the parser already
    // refused it together with the CE-1600P.
    if (preset.interfaceName == "ce158") {
        if (!BundledRoms::attachCE158(machine, romDirs, &result.error)) {
            if (log) log(result.error);
            return result;
        }
        result.ce158Attached = true;
        if (log) log("interface: CE-158 attached (LH5803 side)");
    }

    // Machine is now fully armed (model/cards/plotter wired) but still
    // powered off -- give the caller a chance to repaint that state before
    // the boot below makes it start running.
    if (onArmed) onArmed(result);

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
