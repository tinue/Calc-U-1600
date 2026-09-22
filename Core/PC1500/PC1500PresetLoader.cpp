#include "PC1500PresetLoader.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>

#include "../HostClock.hpp"
#include "../TraceTypes.hpp"

#include "../Connector/MemoryCardCatalog.hpp"
#include "../Connector/SoftwareDefinedCard.hpp"
#include "../Resources/BundledRomCatalog.hpp"
#include "../Basic/BasicProgramSource.hpp"
#include "PC1500BasicLoader.hpp"
#include "PC1500BasicTyper.hpp"
#include "PC1500Machine.hpp"
#include "PC1500Screenshot.hpp"

namespace {

// ~2.6MHz crystal / 2 -- same documented value as Upd1990ac.hpp's own
// kCpuHz (duplicated locally, same as PC1500BasicTyper.cpp does).
constexpr double kCpuHz = 1300000.0;
constexpr int kFramesPerSecond = 60;
constexpr uint64_t kCyclesPerFrame = static_cast<uint64_t>(kCpuHz / kFramesPerSecond);

// Generous margin past the ROM's own power-on RAM-check/boot sequence --
// keys sent immediately after reset are missed entirely, since the ROM
// doesn't start polling the keyboard until it settles into its post-boot
// idle loop.
constexpr uint64_t kBootSettleCycles = static_cast<uint64_t>(kCpuHz * 2);
constexpr uint64_t kIdleCap = static_cast<uint64_t>(kCpuHz * 5);
constexpr uint64_t kProgramIdleCap = static_cast<uint64_t>(kCpuHz * 3600);  // 1 h emulated

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

std::string stepTag(PC1500Machine& machine) {
    // std::string, not a fixed buffer -- screenText() can return up to 80
    // chars and truncating it would blank exactly the diagnostic (what
    // ended up on the LCD edit line) this tag exists to show.
    return "  screen=\"" + screenText(machine) + "\" pc=" + hex4(machine.debugPC());
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

bool runSteps(PC1500Machine& machine, const std::vector<PresetStep>& steps, std::string* error,
              const PresetLogFn& log, const std::string& traceDir, const PresetSaveAsFn& onSaveAs) {
    for (const PresetStep& step : steps) {
        switch (step.kind) {
            case PresetStep::Kind::Key:
                if (step.text == "break" || step.text == "on") {
                    tapBreak(machine);
                } else {
                    tapKey(machine, step.text);
                }
                if (log) log("  key: " + step.text + stepTag(machine));
                break;
            case PresetStep::Kind::Type:
                if (log) log("  type: \"" + step.text + "\" ...");
                if (!typeLine(machine, step.text, /*pressEnter=*/true, error)) {
                    if (log) log("  type: \"" + step.text + "\" FAILED: " + (error ? *error : ""));
                    return false;
                }
                if (log) log("  type: \"" + step.text + "\" done" + stepTag(machine));
                break;
            case PresetStep::Kind::Wait: {
                if (step.waitSeconds < 0) {
                    // Parameterless `- wait:` -- block until the interpreter is
                    // back in its idle command loop (a long RUN / plot done).
                    // A short unconditional lead-in first, so a RUN / plot that
                    // hasn't spun up yet (interpreter momentarily back at the
                    // prompt right after the last typed step) can't trip an
                    // instant false "idle".
                    constexpr uint64_t kLeadIn = static_cast<uint64_t>(kCpuHz / 2);          // 0.5 s emulated
                    constexpr uint64_t kUntilIdleCap = static_cast<uint64_t>(kCpuHz * 3600); // 1 h emulated
                    uint64_t spent = machine.runCycles(kLeadIn);
                    spent += waitUntilBasicIdle(machine, kUntilIdleCap);
                    if (log) {
                        char secs[24];
                        std::snprintf(secs, sizeof(secs), "%.1f", spent / kCpuHz);
                        log("  wait: (until idle, " + std::string(secs) +
                            (spent >= kUntilIdleCap ? "s -- CAP HIT)" : "s)") + stepTag(machine));
                    }
                } else {
                    machine.runCycles(static_cast<uint64_t>(step.waitSeconds * kCpuHz));
                    if (log) {
                        char secs[16];
                        std::snprintf(secs, sizeof(secs), "%.3g", step.waitSeconds);
                        log("  wait: " + std::string(secs) + "s" + stepTag(machine));
                    }
                }
                break;
            }
            case PresetStep::Kind::Trace: {
                // Port of Calc-U-59's `KEYSTROKES:` `Trace:` directive.
                // Empty text -> stop; otherwise (re)start a capture to
                // <traceDir>/<text>. Starting a new one first closes any
                // open one (matching Calc-U-59); an open capture left
                // running is closed by applyPC1500Preset()'s TraceCloser guard.
                if (machine.cpuTraceActive()) machine.endCpuTrace();
                if (step.text.empty()) {
                    if (log) log("  trace: stopped");
                    break;
                }
                std::string path = traceDir + "/" + step.text;
                errno = 0;
                std::FILE* fh = std::fopen(path.c_str(), "wb");
                if (!fh) {
                    if (error)
                        *error = "could not open trace file: " + path + " (" + std::strerror(errno) + ")";
                    if (log) log("  trace: FAILED to open " + path + ": " + std::strerror(errno));
                    return false;
                }
                machine.beginCpuTrace(fh, TRACE_PC | TRACE_REGS_LIGHT | TRACE_REGS_FULL);
                if (log) log("  trace: started -> " + path);
                break;
            }
            case PresetStep::Kind::Screenshot: {
                // PNG of the dot matrix, as it stands right now, into the
                // trace directory (same image as the GUI's Copy Screen).
                const std::string path = traceDir + "/" + step.text;
                std::string writeError;
                if (!writeLcdScreenshotPng(pc1500LcdBitmap(machine), kPC1500ScreenMm, path, &writeError)) {
                    if (error) *error = "screenshot: " + writeError;
                    if (log) log("  screenshot: FAILED: " + writeError);
                    return false;
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
            case PresetStep::Kind::SaveAs: {
                if (!onSaveAs) {
                    if (log) log("  saveas: skipped (no save handler configured)");
                    break;
                }
                std::string saveError;
                if (!onSaveAs(step.saveAsTarget, step.text, &saveError)) {
                    if (error) *error = "saveas: " + saveError;
                    if (log) log("  saveas: FAILED: " + saveError);
                    return false;
                }
                if (log) log("  saveas: -> \"" + step.text + "\"");
                break;
            }
        }
    }
    return true;
}

// Rough line count for a progress-log string only (blank lines included,
// trailing newline or not). The authoritative typed-line accounting is
// typeBasicProgramText()'s own.
int countLines(const std::string& text) {
    if (text.empty()) return 0;
    int n = static_cast<int>(std::count(text.begin(), text.end(), '\n'));
    return text.back() == '\n' ? n : n + 1;
}

} // namespace

PresetLoadResult applyPC1500Preset(PC1500Machine& machine, const PresetFile& preset,
                              const PresetLogFn& log,
                              const std::string& traceDir, const std::string& moduleDir,
                              const PresetBootedFn& onBooted, const std::vector<std::string>& romDirs,
                              const std::vector<std::string>& extraModuleDirs,
                              const PresetArmedFn& onArmed,
                              const PresetSaveAsFn& onSaveAs) {
    PresetLoadResult result;

    // A trace started by a `- trace:` step and never explicitly stopped
    // is closed (SESSION_END written, file closed) when this function
    // returns, by ANY path -- mirrors Calc-U-59's own auto-close of a
    // scripted trace left open past the end of a KEYSTROKES sequence.
    struct TraceCloser {
        PC1500Machine& m;
        ~TraceCloser() { if (m.cpuTraceActive()) m.endCpuTrace(); }
    } traceCloser{machine};

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
        std::string err;
        std::string specPath = preset.memoryExpansionModuleSpecFile;
        std::vector<std::string> moduleDirs{moduleDir};
        moduleDirs.insert(moduleDirs.end(), extraModuleDirs.begin(), extraModuleDirs.end());
        if (specPath.empty() &&
            !resolveModuleSpecByName(moduleDirs, preset.memoryExpansionModuleSpecName, &specPath,
                                     &err)) {
            result.error = "memory-expansion modulespec: " + err;
            if (log) log("modulespec load FAILED: " + err);
            return result;
        }
        auto card = makeSoftwareDefinedCard(specPath, host, &err);
        if (!card) {
            result.error = "memory-expansion modulespec: " + err;
            if (log) log("modulespec load FAILED: " + err);
            return result;
        }
        machine.attachExpansionCard(std::move(card));
        result.expansionModuleResolvedPath = specPath;
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

    if (log) log("reset + boot settle done" + stepTag(machine));
    if (onBooted) onBooted();

    int sectionNo = 0;
    const int sectionCount = static_cast<int>(preset.sections.size());
    for (const PresetSection& section : preset.sections) {
        sectionNo++;
        char hdr[64];
        if (section.kind == PresetSection::Kind::Keys) {
            if (log) {
                std::snprintf(hdr, sizeof(hdr), "section %d/%d: keys (%zu step%s)", sectionNo, sectionCount,
                              section.keys.size(), section.keys.size() == 1 ? "" : "s");
                log(hdr);
            }
            if (!runSteps(machine, section.keys, &result.error, log, traceDir, onSaveAs)) return result;
            continue;
        }

        // Kind::Program. A preceding `type:` step returns right after its
        // ENTER, so a command it started (e.g. a SAVE) may still be running
        // -- let it finish before this section types or pokes a program
        // into memory underneath it.
        waitUntilBasicIdle(machine, kProgramIdleCap);
        const PresetProgram& program = section.program;
        if (program.format == PresetProgram::Format::BasicText) {
            if (log) {
                std::snprintf(hdr, sizeof(hdr), "section %d/%d: program (basic-text, %d lines)", sectionNo,
                              sectionCount, countLines(program.text));
                log(hdr);
            }
            BasicTypeResult typed = typeBasicProgramText(machine, program.text);
            for (const std::string& rejected : typed.rejectedLines) {
                result.rejectedBasicLines.push_back(rejected);
                if (log) log("  REJECTED by ROM: " + rejected);
            }
            if (!typed.ok) {
                result.error = typed.error;
                if (log) log("  program typing FAILED: " + typed.error);
                return result;
            }
            if (log) log("  program typed OK" + stepTag(machine));
        } else if (program.format == PresetProgram::Format::BasicBinary) {
            basic::BasicProgramSource src =
                basic::readBasicProgramSource(program.path, basic::TransferModel::PC1500);
            if (!src.ok) {
                result.error = src.error;
                if (log) log("section " + std::to_string(sectionNo) + ": basic-binary FAILED: " + result.error);
                return result;
            }
            if (log) {
                std::snprintf(hdr, sizeof(hdr),
                              "section %d/%d: program (basic-binary, %zu tokenized bytes)",
                              sectionNo, sectionCount, src.payload.size());
                log(hdr);
            }
            PC1500BasicLoadResult loaded = loadBasicBinaryPayload(machine, src.payload);
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
        } else {
            errno = 0;
            std::ifstream in(program.path, std::ios::binary);
            if (!in) {
                // errno detail -- "Permission denied" (a sandbox-denied
                // path, e.g. a preset's sibling binary the file picker
                // never granted access to) and "No such file or
                // directory" (a genuinely missing/mistyped path) look
                // identical from `!in` alone otherwise, and are easy to
                // conflate when debugging a load failure from the app's
                // console.
                result.error =
                    "failed to open program file: " + program.path + " (" + std::strerror(errno) + ")";
                if (log) log("section " + std::to_string(sectionNo) + ": binary FAILED: " + result.error);
                return result;
            }
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            uint16_t addr = program.address;
            for (uint8_t b : bytes) {
                machine.memory().poke(addr, b);
                addr++;
            }
            if (log) {
                char n[48];
                std::snprintf(n, sizeof(n), "section %d/%d: binary ", sectionNo, sectionCount);
                log(std::string(n) + program.path + " (" + std::to_string(bytes.size()) + " bytes) -> " +
                    hex4(program.address));
            }
        }
    }

    result.ok = true;
    if (log) log("preset applied OK" + stepTag(machine));
    return result;
}
