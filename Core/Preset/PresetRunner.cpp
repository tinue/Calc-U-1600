#include "PresetRunner.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "../Basic/BasicProgramSource.hpp"
#include "../Connector/MemoryCardCatalog.hpp"
#include "../Connector/SoftwareDefinedCard.hpp"
#include "../TraceTypes.hpp"

std::vector<std::string> presetModuleDirs(const std::string& moduleDir,
                                          const std::vector<std::string>& extraModuleDirs) {
    std::vector<std::string> dirs{moduleDir};
    dirs.insert(dirs.end(), extraModuleDirs.begin(), extraModuleDirs.end());
    return dirs;
}

std::unique_ptr<ExpansionCard> makePresetModuleCard(const std::string& specFile, const std::string& specName,
                                                    const std::vector<std::string>& moduleDirs, CardHost host,
                                                    std::string* resolvedPath, std::string* error) {
    std::string specPath = specFile;
    if (specPath.empty() && !resolveModuleSpecByName(moduleDirs, specName, &specPath, error)) return nullptr;
    std::unique_ptr<ExpansionCard> card = makeSoftwareDefinedCard(specPath, host, error);
    if (card) *resolvedPath = specPath;
    return card;
}

namespace {

constexpr uint64_t kSecondsPerHour = 3600;

// Rough line count for a progress-log string only (blank lines included,
// trailing newline or not). The authoritative typed-line accounting is the
// typer's own.
int countLines(const std::string& text) {
    if (text.empty()) return 0;
    int n = static_cast<int>(std::count(text.begin(), text.end(), '\n'));
    return text.back() == '\n' ? n : n + 1;
}

bool runSteps(PresetMachine& machine, const std::vector<PresetStep>& steps, std::string* error,
              const PresetLogFn& log, const std::string& traceDir, const PresetSaveAsFn& onSaveAs) {
    const double hz = machine.cyclesPerSecond();
    for (const PresetStep& step : steps) {
        switch (step.kind) {
            case PresetStep::Kind::Key:
                if (!machine.key(step.text, error)) {
                    if (log) log("  key: " + step.text + " FAILED: " + *error);
                    return false;
                }
                if (log) log("  key: " + step.text + machine.stepTag());
                break;
            case PresetStep::Kind::Type:
                if (log) log("  type: \"" + step.text + "\" ...");
                if (!machine.typeLine(step.text, error)) {
                    if (log) log("  type: \"" + step.text + "\" FAILED: " + *error);
                    return false;
                }
                if (log) log("  type: \"" + step.text + "\" done" + machine.stepTag());
                break;
            case PresetStep::Kind::Wait: {
                if (step.waitSeconds < 0) {
                    // Parameterless `- wait:` -- block until the interpreter is
                    // back in its idle command loop (a long RUN / plot done).
                    // A short unconditional lead-in first, so a RUN / plot that
                    // hasn't spun up yet (interpreter momentarily back at the
                    // prompt right after the last typed step) can't trip an
                    // instant false "idle".
                    const uint64_t leadIn = static_cast<uint64_t>(hz / 2);                   // 0.5 s emulated
                    const uint64_t untilIdleCap = static_cast<uint64_t>(hz) * kSecondsPerHour;  // 1 h emulated
                    uint64_t spent = machine.runCycles(leadIn);
                    spent += machine.waitUntilBasicIdle(untilIdleCap);
                    if (log) {
                        char secs[24];
                        std::snprintf(secs, sizeof(secs), "%.1f", spent / hz);
                        log("  wait: (until idle, " + std::string(secs) +
                            (spent >= untilIdleCap ? "s -- CAP HIT)" : "s)") + machine.stepTag());
                    }
                } else {
                    machine.runCycles(static_cast<uint64_t>(step.waitSeconds * hz));
                    if (log) {
                        char secs[16];
                        std::snprintf(secs, sizeof(secs), "%.3g", step.waitSeconds);
                        log("  wait: " + std::string(secs) + "s" + machine.stepTag());
                    }
                }
                break;
            }
            case PresetStep::Kind::Trace: {
                // Port of Calc-U-59's `KEYSTROKES:` `Trace:` directive.
                // Empty text -> stop; otherwise (re)start a capture to
                // <traceDir>/<text>. Starting a new one first closes any
                // open one (matching Calc-U-59); one left running is closed
                // by runPresetSections()'s TraceCloser guard.
                if (machine.cpuTraceActive()) machine.endCpuTrace();
                if (step.text.empty()) {
                    if (log) log("  trace: stopped");
                    break;
                }
                const std::string path = traceDir + "/" + step.text;
                errno = 0;
                std::FILE* fh = std::fopen(path.c_str(), "wb");
                if (!fh) {
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
                if (!machine.writeScreenshot(path, &writeError)) {
                    *error = "screenshot: " + writeError;
                    if (log) log("  screenshot: FAILED: " + writeError);
                    return false;
                }
                if (log) log("  screenshot: -> " + path);
                break;
            }
            case PresetStep::Kind::SyncClock: {
                const std::tm t = machine.syncClock();
                char stamp[32];
                std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &t);
                if (log) log(std::string("  syncclock: -> ") + stamp);
                break;
            }
            case PresetStep::Kind::SaveAs:
                if (!runPresetSaveAsStep(step, onSaveAs, log, error)) return false;
                break;
        }
    }
    return true;
}

bool runProgram(PresetMachine& machine, const PresetProgram& program, const std::string& tag,
                PresetLoadResult* result, const PresetLogFn& log) {
    switch (program.format) {
        case PresetProgram::Format::BasicText: {
            if (log) log(tag + "program (basic-text, " + std::to_string(countLines(program.text)) + " lines)");
            BasicTypeResult typed = machine.typeBasicProgram(program.text);
            for (const std::string& rejected : typed.rejectedLines) {
                result->rejectedBasicLines.push_back(rejected);
                if (log) log("  REJECTED: " + rejected);
            }
            if (!typed.ok) {
                result->error = typed.error;
                if (log) log("  program typing FAILED: " + typed.error);
                return false;
            }
            if (log) log("  program typed OK" + machine.stepTag());
            return true;
        }
        case PresetProgram::Format::BasicBinary: {
            basic::BasicProgramSource src = basic::readBasicProgramSource(program.path, machine.transferModel());
            if (!src.ok) {
                result->error = tag + src.error;
                if (log) log(tag + "basic-binary FAILED: " + src.error);
                return false;
            }
            if (log) log(tag + "program (basic-binary, " + std::to_string(src.payload.size()) + " tokenized bytes)");
            BasicLoadResult loaded = machine.loadBasicPayload(src.payload);
            if (!loaded.ok) {
                result->error = "basic-binary load failed: " + loaded.error;
                if (log) log("  " + result->error);
                return false;
            }
            if (log) {
                char n[96];
                std::snprintf(n, sizeof(n), "  program loaded OK: $%04X..$%04X", loaded.baseAddr, loaded.endAddr);
                log(std::string(n) + machine.stepTag());
            }
            return true;
        }
        case PresetProgram::Format::Binary:
            if (!machine.loadBinary(program, tag, log, &result->error)) {
                if (log) log("  " + result->error);
                return false;
            }
            return true;
    }
    return false;
}

}  // namespace

void runPresetSections(PresetMachine& machine, const PresetFile& preset, PresetLoadResult* result,
                       const PresetLogFn& log, const std::string& traceDir, const PresetSaveAsFn& onSaveAs) {
    // A trace started by a `- trace:` step and never explicitly stopped is
    // closed (SESSION_END written, file closed) when the sections are done,
    // by ANY path -- mirrors Calc-U-59's own auto-close of a scripted trace
    // left open past the end of a KEYSTROKES sequence.
    struct TraceCloser {
        PresetMachine& m;
        ~TraceCloser() {
            if (m.cpuTraceActive()) m.endCpuTrace();
        }
    } traceCloser{machine};

    const uint64_t programIdleCap = static_cast<uint64_t>(machine.cyclesPerSecond()) * kSecondsPerHour;
    const int sectionCount = static_cast<int>(preset.sections.size());
    int sectionNo = 0;
    for (const PresetSection& section : preset.sections) {
        sectionNo++;
        const std::string tag =
            "section " + std::to_string(sectionNo) + "/" + std::to_string(sectionCount) + ": ";
        if (section.kind == PresetSection::Kind::Keys) {
            const size_t n = section.keys.size();
            if (log) log(tag + "keys (" + std::to_string(n) + (n == 1 ? " step)" : " steps)"));
            if (!runSteps(machine, section.keys, &result->error, log, traceDir, onSaveAs)) return;
            continue;
        }
        // A preceding `type:` step returns right after its ENTER, so a
        // command it started (e.g. a SAVE) may still be running -- let it
        // finish before this section types or pokes a program into memory
        // underneath it.
        machine.waitUntilBasicIdle(programIdleCap);
        if (!runProgram(machine, section.program, tag, result, log)) return;
    }

    result->ok = true;
    if (log) log("preset applied OK" + machine.stepTag());
}
