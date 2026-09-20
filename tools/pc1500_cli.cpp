// Headless CLI harness for the PC-1500A core. Boots the given ROM, runs
// it for a fixed cycle budget, and reports:
//   - whether execution stayed within the CPU's addressable/documented
//     opcode space (no crash) and made forward progress,
//   - a PC-visit histogram used to detect convergence onto a small
//     repeating loop (the idle loop signature — this build does not decode
//     the dot-matrix LCD's pixel format, so "idle" is inferred structurally
//     rather than by reading displayed text),
//   - a demonstration of the trace ring buffer and a breakpoint actually
//     halting execution.
//
// Usage: pc1500_cli <rom-file> [maxCycles]
//        pc1500_cli --preset <preset-file.pc1500> [maxCycles] [--modules-dir <dir>] [--wav <out.wav>]
//
// The --preset form parses and applies a `.pc1500` scenario file
// (PresetFile.hpp/PC1500PresetLoader.hpp) instead of a bare ROM --
// firmware, pre-load-keys, program load, and post-load-keys are all
// driven from the preset before falling into the same run loop below.
//
// --modules-dir <dir> is a directory a preset's `- modulespec:
// <module-name>` memory-expansion reference is looked up in (default
// `Calc-U-1600/Resources`, the repo's bundled-module directory -- same
// cwd-relative convention as `roms/`). Repeat it to add fallback
// directories, searched in the order given after the first. A
// `- modulespecfile: <path>` reference ignores it.
//
// --wav <out.wav> records the buzzer (PC6, see PiezoSampler.hpp) for the
// whole run -- preset script included -- as 48 kHz mono 16-bit PCM.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../Core/Audio/WavFile.hpp"
#include "../Core/PC1500/PC1500Machine.hpp"
#include "../Core/PC1500/PresetFile.hpp"
#include "../Core/PC1500/PC1500PresetLoader.hpp"

int main(int argc, char** argv) {
    // Pull an optional `--modules-dir <dir>` out of argv up front so the
    // rest of the parsing keeps its simple fixed positions.
    std::string moduleDir = "Calc-U-1600/Resources";
    std::vector<std::string> extraModuleDirs;  // 2nd+ `--modules-dir`, searched after `moduleDir`
    bool moduleDirSet = false;
    bool dumpBasic = false;
    std::string wavPath;
    {
        std::vector<char*> kept;
        for (int i = 0; i < argc; ++i) {
            if (std::strcmp(argv[i], "--wav") == 0 && i + 1 < argc) {
                wavPath = argv[++i];
                continue;
            }
            if (std::strcmp(argv[i], "--modules-dir") == 0 && i + 1 < argc) {
                if (!moduleDirSet) { moduleDir = argv[++i]; moduleDirSet = true; }
                else               { extraModuleDirs.push_back(argv[++i]); }
                continue;
            }
            if (std::strcmp(argv[i], "--dump-basic") == 0) {
                dumpBasic = true; // read-only BASIC program-area / pointer dump
                continue;
            }
            kept.push_back(argv[i]);
        }
        argc = static_cast<int>(kept.size());
        for (int i = 0; i < argc; ++i) argv[i] = kept[i];
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <rom-file> [maxCycles]\n", argv[0]);
        std::fprintf(stderr, "       %s --preset <preset-file.pc1500> [maxCycles]\n", argv[0]);
        std::fprintf(stderr, "       options: --modules-dir <dir>  --dump-basic  --wav <out.wav>\n");
        return 1;
    }

    uint64_t maxCycles = 2'000'000ull;
    bool usingPreset = std::string(argv[1]) == "--preset";

    // Parsed before the machine is constructed (--preset only) so the
    // machine can be built with the preset's own model/variant -- variant
    // is fixed at PC1500Machine construction, not settable afterward (see
    // PC1500Memory.hpp).
    PresetFile preset;
    if (usingPreset) {
        if (argc < 3) {
            std::fprintf(stderr, "usage: %s --preset <preset-file.pc1500> [maxCycles]\n", argv[0]);
            return 1;
        }
        std::string presetPath = argv[2];
        if (argc >= 4) maxCycles = std::strtoull(argv[3], nullptr, 10);

        std::string error;
        if (!parsePresetFile(presetPath, &preset, &error)) {
            std::fprintf(stderr, "failed to parse preset '%s': %s\n", presetPath.c_str(), error.c_str());
            return 1;
        }
    }
    PC1500Machine machine(usingPreset ? preset.variant : PC1500Variant::PC1500A);

    // Enabled before either branch below (not just before the tail run
    // loop) so a --preset run's own script -- typing the program, RUN,
    // BREAK, and any post-load-keys -- is traced too, not just whatever
    // the tail loop executes afterward. That script is usually the whole
    // point of a debug preset.
    machine.setTraceFlags(TRACE_PC | TRACE_REGS_LIGHT | TRACE_BREAKPOINTS);

    // --wav: drain the buzzer audio as the run goes (the sampler only
    // buffers ~1 s). The yield hook covers the preset script's own
    // runCycles(); the tail loop below drains explicitly.
    std::vector<int16_t> wav;
    auto drainWav = [&] {
        int16_t chunk[4096];
        size_t n;
        while ((n = machine.drainAudio(chunk, 4096)) > 0) wav.insert(wav.end(), chunk, chunk + n);
    };
    if (!wavPath.empty()) machine.setYieldHook(drainWav, 1300000 / 20);

    if (usingPreset) {
        std::string presetPath = argv[2];
        PresetLoadResult loaded = applyPC1500Preset(
            machine, preset,
            [](const std::string& line) { std::fprintf(stderr, "[preset] %s\n", line.c_str()); },
            /*traceDir=*/".",  // a `- trace: name.bin` step writes ./name.bin (same cwd convention as roms/)
            moduleDir, /*onBooted=*/{}, /*romDirs=*/{"roms"}, extraModuleDirs);
        for (const std::string& rejected : loaded.rejectedBasicLines) {
            std::fprintf(stderr, "preset: ROM rejected BASIC line: %s\n", rejected.c_str());
        }
        if (!loaded.ok) {
            std::fprintf(stderr, "failed to apply preset '%s': %s\n", presetPath.c_str(), loaded.error.c_str());
            return 1;
        }
        std::printf("Preset '%s' applied successfully.\n", presetPath.c_str());
    } else {
        std::string romPath = argv[1];
        if (argc >= 3) maxCycles = std::strtoull(argv[2], nullptr, 10);
        if (!machine.loadROMFile(romPath)) {
            std::fprintf(stderr, "failed to load ROM: %s (expected exactly 16384 bytes)\n", romPath.c_str());
            return 1;
        }
        machine.reset();
        std::printf("Reset vector -> P=0x%04X\n", machine.cpu().pc());
    }

    // PC space is a fixed 64K, so a flat array beats a map for this: O(1)
    // increments with no per-instruction allocation over a multi-million-
    // step run.
    static std::array<uint64_t, 65536> pcHistogram{};
    uint64_t consumed = 0;
    uint64_t steps = 0;
    while (consumed < maxCycles) {
        uint16_t pcBefore = machine.cpu().pc();
        int c = machine.step();
        if (c == 0) {
            if (machine.consumeBreakpointHit()) {
                std::printf("breakpoint hit at PC=0x%04X after %llu cycles\n", pcBefore, (unsigned long long)consumed);
                break;
            }
            if (machine.cpu().halted()) {
                std::printf("CPU halted (HLT) at PC=0x%04X after %llu cycles with no pending interrupt\n", pcBefore, (unsigned long long)consumed);
                break;
            }
        }
        if (machine.cpu().consumeIllegalOpcodeHit()) {
            std::printf("illegal opcode 0x%04X at PC=0x%04X after %llu cycles\n",
                        machine.cpu().lastIllegalOpcode(), machine.cpu().lastIllegalOpcodePC(), (unsigned long long)consumed);
        }
        pcHistogram[pcBefore]++;
        consumed += static_cast<uint64_t>(c);
        steps++;
        if (!wavPath.empty() && (steps & 0x3FFF) == 0) drainWav();
    }
    if (!wavPath.empty()) {
        machine.setYieldHook({}, 0);
        drainWav();
        if (!writeWavMono16(wavPath, wav, machine.audioSampleRate())) {
            std::fprintf(stderr, "failed to write '%s'\n", wavPath.c_str());
            return 1;
        }
        std::printf("Wrote %zu samples (%.2f s) of buzzer audio to %s\n", wav.size(),
                    static_cast<double>(wav.size()) / machine.audioSampleRate(), wavPath.c_str());
    }

    std::printf("Ran %llu instructions, %llu cycles\n", (unsigned long long)steps, (unsigned long long)consumed);

    // Convergence signal: an idle loop revisits a small set of addresses a
    // large number of times. Report the most-visited PC and how many
    // distinct addresses were touched at all.
    uint64_t topCount = 0;
    uint32_t topPC = 0;
    uint32_t distinctPCs = 0;
    for (uint32_t pc = 0; pc < pcHistogram.size(); pc++) {
        if (pcHistogram[pc] == 0) continue;
        distinctPCs++;
        if (pcHistogram[pc] > topCount) { topCount = pcHistogram[pc]; topPC = pc; }
    }
    std::printf("Most-visited PC: 0x%04X (%llu visits, %u distinct PCs total)\n",
                topPC, (unsigned long long)topCount, distinctPCs);
    if (distinctPCs < 200 && topCount > steps / 10) {
        std::printf("Signature matches a converged small loop (idle-loop-like behavior).\n");
    }

    std::printf("Final registers: A=0x%02X X=0x%04X Y=0x%04X U=0x%04X S=0x%04X P=0x%04X T=0x%02X (C=%d IE=%d Z=%d V=%d H=%d)\n",
                machine.cpu().a(), machine.cpu().x(), machine.cpu().y(), machine.cpu().u(),
                machine.cpu().sp(), machine.cpu().pc(), machine.cpu().statusReg(),
                machine.cpu().flagC(), machine.cpu().flagIE(), machine.cpu().flagZ(),
                machine.cpu().flagV(), machine.cpu().flagH());

    // Drain trace ring buffer and print the last few instructions executed.
    // The ring only ever retains its own fixed capacity (512, see
    // LH5801::kRingSize) regardless of how many instructions actually ran
    // -- draining fewer than that here would return the *oldest* surviving
    // entries, not the most recent ones (drainTraceEvents advances forward
    // from the oldest undrained event), so this must request the full
    // ring to actually see the tail (e.g. a --preset run's final `up`
    // keypress) rather than some arbitrary earlier slice of it.
    CpuFrame frames[512];
    uint32_t lost = 0;
    uint32_t n = machine.drainTraceEvents(frames, 512, &lost);
    std::printf("Drained %u trace frames (%u lost to overflow before this drain)\n", n, lost);
    for (uint32_t i = n >= 32 ? n - 32 : 0; i < n; i++) {
        std::printf("  seq=%u pc=0x%04X opcode=0x%04X cycles=%u A=0x%02X T=0x%02X\n",
                    frames[i].seqno, frames[i].pc, frames[i].opcode, frames[i].cycles, frames[i].a, frames[i].t);
    }

    // Status icons + the input-line text buffer -- lets a --preset run's
    // final keystrokes (e.g. the up-key-debug scenario's trailing
    // `key: up`) be inspected without decoding the dot-matrix LCD's pixel
    // format, same rationale as pc1500emu's own "displaytext" command
    // (see PC1500BasicTyper.cpp's port-source comment). This is the last
    // *typed input line*, not necessarily what a statement evaluated to
    // (see preset_file_format.md's own "check" step doc for that
    // distinction) -- still useful here since PRO-mode Up/Down is a line-
    // editor navigation key, which is exactly what this buffer reflects.
    PC1500Display display = machine.display();
    std::printf("Display: busy=%d pro=%d run=%d shift=%d sml=%d\n",
                display.busy(), display.pro(), display.run(), display.shift(), display.small());
    static constexpr uint16_t kDisplayTextBufBase = 0x7BB0;
    static constexpr int kDisplayTextBufLen = 80;
    std::string displayText;
    for (int i = 0; i < kDisplayTextBufLen; i++) {
        uint8_t b = machine.memory().peek(static_cast<uint16_t>(kDisplayTextBufBase + i));
        if (b == 0x0D) break;
        displayText += (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '?';
    }
    std::printf("Display input-line text: \"%s\"\n", displayText.c_str());

    if (dumpBasic) {
        // Read-only oracle for the fast BASIC loader work (see
        // ~/.claude/plans/goal-faster-basic-program-woolly-wall.md): dump the
        // BASIC program pointers and the raw bytes the ROM's line editor
        // actually laid down, so a direct-poke loader can reproduce them.
        auto be16 = [&](uint16_t a) {
            return static_cast<uint16_t>((machine.memory().peek(a) << 8) |
                                         machine.memory().peek(static_cast<uint16_t>(a + 1)));
        };
        uint16_t st = be16(0x7865), end = be16(0x7867), edt = be16(0x7869);
        uint16_t varStart = be16(0x7899), dataPtr = be16(0x78BE);
        std::printf("--- BASIC pointers ---\n");
        std::printf("BASPRG_ST  $7865 = $%04X\n", st);
        std::printf("BASPRG_END $7867 = $%04X   (len = %d)\n", end, static_cast<int>(end) - static_cast<int>(st));
        std::printf("BASPRG_EDT $7869 = $%04X\n", edt);
        std::printf("VAR_START  $7899 = $%04X\n", varStart);
        std::printf("DATA_PTR   $78BE = $%04X\n", dataPtr);
        std::printf("RAM_END    $7864 = $%02X (page)   WARM_START $7A20 = $%02X\n",
                    machine.memory().peek(0x7864), machine.memory().peek(0x7A20));
        std::printf("--- program area $%04X..$%04X+16 ---\n", st, end);
        uint16_t lo = st, hi = static_cast<uint16_t>(end + 16);
        for (uint16_t a = lo; a <= hi && a >= lo; a += 16) {
            std::printf("%04X: ", a);
            for (int i = 0; i < 16; i++) std::printf("%02X ", machine.memory().peek(static_cast<uint16_t>(a + i)));
            std::printf(" |");
            for (int i = 0; i < 16; i++) {
                uint8_t b = machine.memory().peek(static_cast<uint16_t>(a + i));
                std::printf("%c", (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.');
            }
            std::printf("|\n");
            if (a > 0xFFF0) break;
        }
    }

    if (machine.ce150Attached()) {
        auto pts = machine.ce150PlotPoints();
        std::printf("CE-150: attached, plot points=%zu revision=%llu\n",
                    pts.size(), static_cast<unsigned long long>(machine.ce150PlotRevision()));
        auto events = machine.drainCE150Events();
        std::printf("CE-150 events (%zu):\n", events.size());
        for (size_t i = 0; i < events.size() && i < 60; ++i)
            std::printf("  %s\n", events[i].c_str());
    }

    return 0;
}
