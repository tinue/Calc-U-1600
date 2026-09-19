#include "PC1600PresetLoader.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "../Connector/FloppyImageFile.hpp"
#include "../Connector/MemoryCardCatalog.hpp"
#include "../Connector/SlotModuleFactory.hpp"
#include "../Connector/SoftwareDefinedCard.hpp"
#include "../Resources/BundledRomCatalog.hpp"
#include "../HostClock.hpp"
#include "../TraceTypes.hpp"
#include "../Basic/BasicProgramSource.hpp"
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

// A `key:` step value: `break`/`on` (the ON key) or a single named PC-1600
// key (PC1600Keyboard::keyFromName). The parser already rejects anything
// else (pointing at `type:`), so this only stays defensive.
bool runKeyStep(PC1600Machine& machine, const std::string& value, std::string* error) {
    // Same gap as between typed lines: after a prior line's ENTER the ROM
    // is out of its key-scan loop for a bit. No-op without a plotter.
    waitForKeyboardScanLoop(machine);
    if (value == "break" || value == "on") {
        tapBreak(machine);
        return true;
    }
    if (PC1600Keyboard::keyFromName(value) != PC1600Keyboard::Key::Unknown) {
        tapKey(machine, value);
        return true;
    }
    *error = "key: '" + value + "' is not a known PC-1600 key";
    return false;
}

// A `type:` step: the line's characters as keystrokes, then ENTER. Case-
// sensitive; lowercase and the shifted punctuation / digit-row symbols go
// via a SHIFT-tap (see PC1600BasicTyper::typeLine).
bool runTypeStep(PC1600Machine& machine, const std::string& text, std::string* error) {
    return typeLine(machine, text, /*pressEnter=*/true, error);
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
bool loadMachineBinary(PC1600Machine& machine, const PresetProgram& program, int sectionNo,
                       const PC1600PresetLogFn& log, std::string* error) {
    const std::string tag = "section " + std::to_string(sectionNo) + ": ";

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
bool attachPresetPlotter(PC1600Machine& machine, const std::string& plotter, const std::string& floppy,
                         int floppySide, const std::vector<std::string>& romDirs,
                         const std::vector<std::string>& moduleDirs, const PC1600PresetLogFn& log,
                         PC1600PresetLoadResult* result) {
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

    if (!BundledRoms::attachPlotterByName(machine, plotter, romDirs, &result->error, &result->ce150Attached)) {
        return false;
    }
    if (!floppy.empty()) {
        machine.ce1600fLoadImage(disk.image.data(), disk.image.size());  // resets to side A
        if (floppySide != 0) machine.ce1600fSetSide(floppySide);
    }
    if (log) {
        log(plotter == "ce150" ? "plotter: CE-150 attached (LH5803 side)"
                                : "plotter: " + plotter + " attached" +
                                      (floppy.empty() ? "" : " (floppy: " + floppy + ")"));
    }
    return true;
}

} // namespace

PC1600PresetLoadResult applyPC1600Preset(PC1600Machine& machine, const PresetFile& preset,
                                         const PC1600PresetLogFn& log, const std::string& traceDir,
                                         const std::string& moduleDir,
                                         const PC1600PresetBootedFn& onBooted,
                                         const std::vector<std::string>& romDirs,
                                         const std::vector<std::string>& extraModuleDirs,
                                         const PC1600PresetArmedFn& onArmed) {
    PC1600PresetLoadResult result;

    // The `- modulespec: <name>` search path: `moduleDir` first (bundled
    // catalogue), then any `extraModuleDirs` (the GUI's iCloud BatteryCards
    // folder). Built once here; the `plug` lambda below captures it.
    std::vector<std::string> moduleDirs{moduleDir};
    moduleDirs.insert(moduleDirs.end(), extraModuleDirs.begin(), extraModuleDirs.end());

    // A trace started by a `- trace:` step and never explicitly stopped is
    // closed (SESSION_END written, file closed) when this function returns
    // by any path -- mirrors PC1500PresetLoader's own TraceCloser.
    struct TraceCloser {
        PC1600Machine& m;
        ~TraceCloser() { if (m.cpuTraceActive()) m.endCpuTrace(); }
    } traceCloser{machine};

    auto plug = [&](const std::string& name, const std::string& specFile,
                    const std::string& specName, int slot) -> bool {
        std::unique_ptr<ExpansionCard> card;
        std::string label;      // human-readable, for the log line
        std::string guiLabel;   // the module-name / built-in name for the GUI button
        std::string resolvedPath;  // on-disk file, if any (modulespec/modulespecfile only)
        if (!specFile.empty() || !specName.empty()) {
            CardHost host = (slot == 1) ? CardHost::PC1600Slot1 : CardHost::PC1600Slot2;
            std::string err;
            std::string specPath = specFile;
            if (specPath.empty() &&
                !resolveModuleSpecByName(moduleDirs, specName, &specPath, &err)) {
                result.error = "slot " + std::to_string(slot) + " modulespec: " + err;
                return false;
            }
            card = makeSoftwareDefinedCard(specPath, host, &err, &guiLabel);
            if (!card) {
                result.error = "slot " + std::to_string(slot) + " modulespec: " + err;
                return false;
            }
            label = "modulespec " + specPath;
            resolvedPath = specPath;
        } else if (!name.empty()) {
            card = makeSlotModuleCard(name);
            if (!card) { // parser already vetted the name, but stay defensive
                result.error = "unknown slot " + std::to_string(slot) + " module '" + name + "'";
                return false;
            }
            label = name;
            guiLabel = name;
        } else {
            return true;  // empty slot
        }
        if (slot == 1) {
            machine.attachSlot1Card(std::move(card));
            result.slot1ModuleLabel = guiLabel;
            result.slot1ResolvedPath = resolvedPath;
        } else {
            machine.attachSlot2Card(std::move(card));
            result.slot2ModuleLabel = guiLabel;
            result.slot2ResolvedPath = resolvedPath;
        }
        if (log) log("slot " + std::to_string(slot) + ": " + label + " attached");
        return true;
    };
    if (!plug(preset.slot1Module, preset.slot1ModuleSpecFile, preset.slot1ModuleSpecName, 1))
        return result;
    if (!plug(preset.slot2Module, preset.slot2ModuleSpecFile, preset.slot2ModuleSpecName, 2))
        return result;

    // Plotter (`plotter:`) -- attach before the reset below, so the boot
    // ROM's peripheral scan sees it (mirrors real hardware: power off,
    // connect, power on).
    if (!attachPresetPlotter(machine, preset.plotter, preset.floppy, preset.floppySide, romDirs, moduleDirs,
                              log, &result))
        return result;

    // Machine is now fully armed (model/cards/plotter wired) but still
    // powered off -- give the caller a chance to repaint that state before
    // the boot below makes it start running.
    if (onArmed) onArmed(result);

    // Full cold boot: the slot config just changed, so the IOCS work area
    // must be rebuilt from scratch (simple reset() would keep stale RAM).
    machine.allReset();
    // Includes the plotter's power-on init -- `waitForKeyboardScanLoop`
    // also guards every typed line (PC1600BasicTyper) and every `key:` step
    // (runKeyStep) for the same gap that reopens after each line's ENTER.
    runBootToPrompt(machine);
    if (log) log("reset + boot settle done" + stepTag(machine));
    if (onBooted) onBooted();

    int sectionNo = 0;
    for (const PresetSection& section : preset.sections) {
        sectionNo++;
        if (section.kind == PresetSection::Kind::Program) {
            // A preceding `type:` step returns right after its ENTER, so a
            // command it started (e.g. a SAVE) may still be running -- let it
            // finish before this section pokes a program into memory
            // underneath it.
            constexpr uint64_t kProgramIdleCap = static_cast<uint64_t>(kTStateHz) * 3600;  // 1 h emulated
            waitUntilBasicIdle(machine, kProgramIdleCap);
            const PresetProgram& program = section.program;
            if (program.format == PresetProgram::Format::BasicBinary) {
                basic::BasicProgramSource src =
                    basic::readBasicProgramSource(program.path, basic::TransferModel::PC1600);
                if (!src.ok) {
                    result.error = "section " + std::to_string(sectionNo) + ": " + src.error;
                    return result;
                }
                if (log)
                    log("section " + std::to_string(sectionNo) + ": program (basic-binary, " +
                        std::to_string(src.payload.size()) + " tokenized bytes)");
                PC1600BasicLoadResult loaded = loadBasicBinaryPayload(machine, src.payload);
                if (!loaded.ok) {
                    result.error = "basic-binary load failed: " + loaded.error;
                    if (log) log("  " + result.error);
                    return result;
                }
                if (log) {
                    char n[96];
                    std::snprintf(n, sizeof(n), "  program loaded OK: $%04X..$%04X", loaded.baseAddr,
                                  loaded.endAddr);
                    log(std::string(n) + stepTag(machine));
                }
                continue;
            }
            if (program.format == PresetProgram::Format::Binary) {
                if (!loadMachineBinary(machine, program, sectionNo, log, &result.error)) {
                    if (log) log("  " + result.error);
                    return result;
                }
                continue;
            }
            if (log) log("section " + std::to_string(sectionNo) + ": program (basic-text)");
            PC1600BasicTypeResult typed = typeBasicProgramText(machine, program.text);
            for (const std::string& rejected : typed.rejectedLines) {
                result.rejectedBasicLines.push_back(rejected);
                if (log) log("  REJECTED (too long): " + rejected);
            }
            if (!typed.ok) {
                result.error = typed.error;
                if (log) log("  program typing FAILED: " + typed.error);
                return result;
            }
            if (log) log("  program typed OK" + stepTag(machine));
            continue;
        }

        for (const PresetStep& step : section.keys) {
            switch (step.kind) {
                case PresetStep::Kind::Key:
                    if (!runKeyStep(machine, step.text, &result.error)) return result;
                    if (log) log("  key: " + step.text + stepTag(machine));
                    break;
                case PresetStep::Kind::Type:
                    if (!runTypeStep(machine, step.text, &result.error)) return result;
                    if (log) log("  type: \"" + step.text + "\"" + stepTag(machine));
                    break;
                case PresetStep::Kind::Wait:
                    if (step.waitSeconds < 0) {
                        // Parameterless `- wait:` -- block until the interpreter
                        // is back in its command loop (a long RUN / plot done).
                        // A short unconditional lead-in first, so a RUN / plot
                        // that hasn't spun up yet can't trip an instant false
                        // "idle".
                        constexpr uint64_t kLeadIn = kTStateHz / 2;  // 0.5 s emulated
                        constexpr uint64_t kUntilIdleCap =
                            static_cast<uint64_t>(kTStateHz) * 3600;  // 1 h emulated
                        uint64_t spent = machine.runCycles(kLeadIn);
                        spent += waitUntilBasicIdle(machine, kUntilIdleCap);
                        if (log) {
                            char secs[24];
                            std::snprintf(secs, sizeof(secs), "%.1f",
                                          spent / static_cast<double>(kTStateHz));
                            log("  wait: (until idle, " + std::string(secs) +
                                (spent >= kUntilIdleCap ? "s -- CAP HIT)" : "s)") + stepTag(machine));
                        }
                    } else {
                        machine.runCycles(static_cast<uint64_t>(step.waitSeconds * kTStateHz));
                        if (log) log("  wait: done" + stepTag(machine));
                    }
                    break;
                case PresetStep::Kind::Trace: {
                    // Port of the PC-1500 loader's `trace:` handling. Empty
                    // text -> stop; otherwise (re)start a capture to
                    // <traceDir>/<text>, first closing any open one.
                    if (machine.cpuTraceActive()) machine.endCpuTrace();
                    if (step.text.empty()) {
                        if (log) log("  trace: stopped");
                        break;
                    }
                    std::string path = traceDir + "/" + step.text;
                    errno = 0;
                    std::FILE* fh = std::fopen(path.c_str(), "wb");
                    if (!fh) {
                        result.error =
                            "could not open trace file: " + path + " (" + std::strerror(errno) + ")";
                        if (log) log("  trace: FAILED to open " + path + ": " + std::strerror(errno));
                        return result;
                    }
                    machine.beginCpuTrace(fh, TRACE_PC | TRACE_REGS_LIGHT | TRACE_REGS_FULL);
                    if (log) log("  trace: started -> " + path);
                    break;
                }
                case PresetStep::Kind::Screenshot: {
                    // PNG of the graphics area, as it stands right now, into
                    // the trace directory (same image as the GUI's Copy Screen).
                    const std::string path = traceDir + "/" + step.text;
                    std::string writeError;
                    if (!writeLcdScreenshotPng(pc1600LcdBitmap(machine), kPC1600ScreenMm, path, &writeError)) {
                        result.error = "screenshot: " + writeError;
                        if (log) log("  screenshot: FAILED: " + writeError);
                        return result;
                    }
                    if (log) log("  screenshot: -> " + path);
                    break;
                }
                case PresetStep::Kind::SyncClock: {
                    const std::tm t = seedClockFromHostTime(machine);
                    char stamp[32];
                    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &t);
                    if (log) log(std::string("  syncclock: -> ") + stamp);
                    break;
                }
            }
        }
    }

    result.ok = true;
    if (log) log("preset applied OK" + stepTag(machine));
    return result;
}
