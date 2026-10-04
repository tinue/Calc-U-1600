// Headless CLI for the PC-1600. The --preset form runs a full
// PC1600Machine through a `.pc1600` preset (below). The plain form boots
// only the SC7852 from romI-0 + romII-0 (no LH5803), runs a fixed cycle
// budget and reports a PC-visit histogram that shows convergence onto a
// small repeating loop, like pc1500_cli.
//
// Usage: pc1600_cli <romI-0-file> <romII-0-file> [maxCycles]
//        pc1600_cli --check-preset <preset-file>...
//        pc1600_cli --preset <preset-file.pc1600> [maxCycles] [--dump-basic] [--modules-dir <dir>] [--save-dir <dir>] [--wav <out.wav>] [--tape-in <in.wav>] [--tape-out <out.wav>] [--dump-mem <addr>,<len>] [--rom new|old] [--ce1600p-rom new|old] [--lcd-png <out.png>] [--lcd-text <out.txt|->]
//
// --check-preset parses each preset (any model) and reports ok / the error,
// without booting anything; tools/check_presets.sh runs it over the repo.
//
// The --preset form loads the confirmed PC-1600 ROM set from roms/ (same
// names pc1600_preset_tests.cpp uses), builds a full PC1600Machine, and
// applies a `.pc1600` scenario via applyPC1600Preset() -- the SC7852-only
// histogram mode below is skipped. --dump-basic then prints the BASIC
// program pointers and the raw program-area bytes (read-only), the oracle
// the fast BASIC loader is checked against.
//
// --rom new|old (--preset only) overrides the preset's PC-1600 ROM version
// (`model: PC-1600:new|old`, default new).
// --ce1600p-rom new|old (--preset only) overrides the preset's CE-1600P ROM
// version (`plotter: CE-1600P:new|old`, default new); independent of --rom.
//
// `- saveas:` steps write cards and floppies like the GUI (Core/PC1600/
// PC1600PresetMedia.hpp): the `file:<path>` form writes that file; a by-name
// save goes to --save-dir <dir> (<dir>/<name>.card.yaml / .floppy.yaml) and
// fails without it.
//
// CE-158 (--preset only, a preset with `interface: CE-158`): --ce158-pty,
// --ce158-rx <file>, --ce158-rx-hold <n>, --ce158-tx <file> -- same as
// pc1500_cli (see its header). --run-after <tstates> keeps the machine
// running that long after the preset script (e.g. to finish a serial
// exchange with a --ce158-rx peer).
//
// --lcd-png <out.png> (--preset only) writes the LCD, as Copy Screen does,
// once the preset (and any --run-after) has finished. --lcd-text <out.txt>
// (or - for stdout) writes it as text at the same point (Core/Display/
// LcdText.hpp): the rows, the lit status symbols, and the number of cells
// that are not text.
//
// --wav <out.wav> (--preset only) records the buzzer (OPC 18H, see
// PiezoSampler.hpp) while the preset script runs, as 48 kHz mono 16-bit
// PCM -- as the host hears it, i.e. through the PC-1600 transducer model.
//
// --tape-in <in.wav> / --tape-out <out.wav> (--preset only) put a cassette
// into the recorder behind the CE-1600P (Core/Tape/TapeDeck.hpp) before the
// preset runs: a WAV to play for the preset's CLOAD, or a blank tape whose
// recording (the preset's CSAVE) is saved to <out.wav> when the motor stops. The
// tape moves only while the CE-1600P's remote relay runs it. The preset
// needs `plotter: CE-1600P`.
//
// --dump-mem <addr>,<len> (--preset only; repeatable) prints that much of
// the Z-80's view of memory as hex once the preset has run, e.g.
// --dump-mem 0xD000,300 after a CLOAD M.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../Core/Audio/WavFile.hpp"
#include "../Core/Connector/FloppyImageFile.hpp"
#include "../Core/CPU/SC7852/SC7852.hpp"
#include "../Core/PC1600/PC1600Bank.hpp"
#include "../Core/PC1600/PC1600Machine.hpp"
#include "../Core/PC1600/PC1600Memory.hpp"
#include "../Core/PC1600/PC1600PresetLoader.hpp"
#include "../Core/PC1600/PC1600PresetMedia.hpp"
#include "../Core/PC1600/PC1600LcdText.hpp"
#include "../Core/PC1600/PC1600Screenshot.hpp"
#include "../Core/Preset/PresetFile.hpp"
#include "../Core/Resources/BundledRomCatalog.hpp"
#include "Ce158CliPeer.hpp"
#include "CliCommon.hpp"

namespace {
int runPreset(const std::string& presetPath, uint64_t maxCycles, bool dumpBasic,
              const std::string& moduleDir, const std::vector<std::string>& extraModuleDirs,
              const std::string& wavPath, const std::string& romOverride,
              const std::string& ce1600pRomOverride, const std::string& saveDir,
              Ce158CliPeer& ce158Peer, uint64_t runAfter, const std::string& lcdPng,
              const std::string& lcdTextPath, const std::string& tapeIn, const std::string& tapeOut,
              const std::vector<std::pair<uint32_t, uint32_t>>& memDumps) {
    (void)maxCycles;
    PresetFile preset;
    std::string error;
    if (!parsePresetFile(presetPath, &preset, &error)) {
        std::fprintf(stderr, "failed to parse preset '%s': %s\n", presetPath.c_str(), error.c_str());
        return 1;
    }
    if (!preset.isPC1600()) {
        std::fprintf(stderr, "preset '%s' is not a PC-1600 preset\n", presetPath.c_str());
        return 1;
    }
    if (!ce1600pRomOverride.empty()) {
        if (!BundledRoms::isCE1600PRomVersion(ce1600pRomOverride)) {
            std::fprintf(stderr, "--ce1600p-rom must be new or old\n");
            return 1;
        }
        preset.ce1600pRomVariant = ce1600pRomOverride;
    }
    PC1600Machine machine;
    std::string romSetError;
    if (!BundledRoms::loadPC1600RomSet(machine, {"roms"},
                                        romOverride.empty() ? preset.romVariant : romOverride,
                                        &romSetError)) {
        std::fprintf(stderr, "could not load the PC-1600 ROM set: %s (run from repo root)\n",
                     romSetError.c_str());
        return 1;
    }
    // --wav: the sampler only buffers ~1 s, so drain it from the yield
    // hook while the preset script runs.
    std::vector<int16_t> wav;
    auto drainWav = [&] {
        int16_t chunk[4096];
        size_t n;
        while ((n = machine.drainAudio(chunk, 4096)) > 0) wav.insert(wav.end(), chunk, chunk + n);
    };
    if (!wavPath.empty()) machine.setYieldHook(drainWav, PC1600Machine::kTStateHz / 20);
    if (!ce158Peer.attach(machine)) return 1; // before the preset attaches the card
    if (!tapeIn.empty()) {
        std::string tapeError;
        if (!machine.tapePlay(tapeIn, tapeError)) {
            std::fprintf(stderr, "--tape-in: %s\n", tapeError.c_str());
            return 1;
        }
    } else if (!tapeOut.empty()) {
        machine.tapeRecord(tapeOut);
    }
    // `saveas:` -- the file form always works; a by-name save needs
    // --save-dir. Cards splice into the file the loader attached them from
    // (reported by onArmed, before any step runs).
    PresetLoadResult armedResult;
    const auto onArmed = [&armedResult](const PresetLoadResult& r) { armedResult = r; };
    const PresetSaveAsFn onSaveAs = [&machine, &saveDir, &armedResult](const PresetSaveAsRequest& request,
                                                                       std::string* err) {
        return savePC1600PresetMedia(machine, request, saveDir, armedResult.slot1ResolvedPath,
                                     armedResult.slot2ResolvedPath, err);
    };
    PresetLoadResult loaded = applyPC1600Preset(
        machine, preset,
        [](const std::string& line) { std::fprintf(stderr, "[preset] %s\n", line.c_str()); }, ".",
        moduleDir,
        /*onBooted=*/{}, /*romDirs=*/{"roms", "firmware/pc1600-hostdrive"}, extraModuleDirs, onArmed, onSaveAs);
    for (const std::string& r : loaded.rejectedBasicLines)
        std::fprintf(stderr, "preset: rejected BASIC line: %s\n", r.c_str());
    if (!loaded.ok && dumpBasic) {
        // Dump the pointer state even on failure so a load error can be
        // diagnosed (e.g. an unexpected BASPRG_ST with an expansion module).
        auto pk = [&](uint16_t a) { return machine.debugPeek(a); };
        std::fprintf(stderr,
                     "[pointers] F865(BE)=$%02X%02X F867(BE)=$%02X%02X F5CF(LE)=$%02X%02X "
                     "F89E(BE)=$%02X%02X\n",
                     pk(0xF865), pk(0xF866), pk(0xF867), pk(0xF868), pk(0xF5D0), pk(0xF5CF),
                     pk(0xF89E), pk(0xF89F));
        // The scattered-placement work-area bytes (PC-1600-BASIC-Program-
        // Placement.md): S0MTb + the five ADTBL bank descriptors say how
        // the firmware spread S0 across module banks + internal RAM.
        std::fprintf(stderr,
                     "[adtbl] S0MTb F02A=$%02X  S1MTb/MBb F016/F018=$%02X/$%02X  "
                     "S2MTb/MBb F020/F022=$%02X/$%02X  ADTBL F1D6..F1DA=%02X %02X %02X %02X %02X\n",
                     pk(0xF02A), pk(0xF016), pk(0xF018), pk(0xF020), pk(0xF022), pk(0xF1D6),
                     pk(0xF1D7), pk(0xF1D8), pk(0xF1D9), pk(0xF1DA));
    }
    if (!loaded.ok) {
        std::fprintf(stderr, "failed to apply preset '%s': %s\n", presetPath.c_str(), loaded.error.c_str());
        return 1;
    }
    std::printf("Preset '%s' applied successfully.\n", presetPath.c_str());
    if (runAfter) machine.runCycles(runAfter);
    if (!lcdPng.empty()) {
        std::string pngError;
        if (!writeLcdScreenshotPng(pc1600LcdBitmap(machine), kPC1600ScreenMm, lcdPng, &pngError)) {
            std::fprintf(stderr, "failed to write '%s': %s\n", lcdPng.c_str(), pngError.c_str());
            return 1;
        }
    }
    if (!lcdTextPath.empty()) {
        std::string textError;
        if (!writeLcdTextReport(pc1600LcdText(machine), lcdTextPath, &textError)) {
            std::fprintf(stderr, "failed to write '%s': %s\n", lcdTextPath.c_str(), textError.c_str());
            return 1;
        }
    }
    cli::printCe150Report(machine);
    if (!ce158Peer.report(machine)) return 1;
    if (!tapeIn.empty() || !tapeOut.empty()) {
        const TapeDeck::Status tape = machine.tapeStatus();
        std::printf("Tape: %.2f s of %.2f s, motor %s\n", tape.position, tape.length, tape.motor ? "on" : "off");
        std::string tapeError;
        if (!machine.tapeEject(&tapeError)) {
            std::fprintf(stderr, "--tape-out: %s\n", tapeError.c_str());
            return 1;
        }
        if (!tapeOut.empty()) std::printf("Wrote %.2f s of tape to %s\n", tape.length, tapeOut.c_str());
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

    for (const auto& [start, length] : memDumps) {
        std::printf("--- memory $%04X+%u ---\n", start, length);
        for (uint32_t a = start; a < start + length && a <= 0xFFFF; a += 16) {
            std::printf("%04X:", a);
            for (uint32_t i = a; i < a + 16 && i < start + length && i <= 0xFFFF; ++i)
                std::printf(" %02X", machine.debugPeek(static_cast<uint16_t>(i)));
            std::printf("\n");
        }
    }

    if (dumpBasic) {
        auto be16 = [&](uint16_t a) {
            return static_cast<uint16_t>((machine.debugPeek(a) << 8) |
                                         machine.debugPeek(static_cast<uint16_t>(a + 1)));
        };
        auto le16 = [&](uint16_t a) {
            return static_cast<uint16_t>(machine.debugPeek(a) |
                                         (machine.debugPeek(static_cast<uint16_t>(a + 1)) << 8));
        };
        uint16_t st = be16(0xF865), end = be16(0xF867), edt = be16(0xF869);
        // F865/F867/F869 hold LH5803-side addresses; the LH5803's
        // $0000-$7FFF aliases the SC7852's $8000-$FFFF, so the Z-80 address
        // (debugPeek's view) is value + $8000: $C0C5 stock ($40C5), $80C5
        // with the program in a Slot-1/Slot-2 RAM module ($00C5).
        auto toZ80 = [](uint16_t a) -> uint16_t {
            return a < 0x8000 ? static_cast<uint16_t>(a + 0x8000) : a;
        };
        uint16_t stZ = toZ80(st), endZ = toZ80(end);
        std::printf("--- BASIC pointers (PC-1600) ---\n");
        std::printf("BASPRG_ST  $F865 = $%04X  (Z-80 $%04X)\n", st, stZ);
        std::printf("BASPRG_END $F867 = $%04X  (Z-80 $%04X, len = %d)\n", end, endZ,
                    static_cast<int>(end) - static_cast<int>(st));
        std::printf("BASPRG_EDT $F869 = $%04X\n", edt);
        std::printf("F5CF (LE, RAM base) = $%04X   F89D (LE) = $%04X\n", le16(0xF5CF), le16(0xF89D));
        std::printf("VARIABLE_PTR $F899 = $%04X\n", be16(0xF899));
        std::printf("--- program area $%04X..$%04X+16 ---\n", stZ, endZ);
        for (uint32_t a = stZ; a <= static_cast<uint32_t>(endZ) + 16 && a <= 0xFFFF; a += 16) {
            std::printf("%04X: ", a);
            for (int i = 0; i < 16; i++)
                std::printf("%02X ", machine.debugPeek(static_cast<uint16_t>(a + i)));
            std::printf(" |");
            for (int i = 0; i < 16; i++) {
                uint8_t b = machine.debugPeek(static_cast<uint16_t>(a + i));
                std::printf("%c", (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.');
            }
            std::printf("|\n");
        }
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    // --check-preset: parse only (parsePresetFile is model-agnostic, so
    // this covers PC-1500 presets too); nothing is booted.
    if (argc >= 3 && std::strcmp(argv[1], "--check-preset") == 0) {
        int failed = 0;
        for (int i = 2; i < argc; ++i) {
            PresetFile preset;
            std::string error;
            if (parsePresetFile(argv[i], &preset, &error)) {
                std::printf("ok    %s\n", argv[i]);
            } else {
                std::printf("FAIL  %s: %s\n", argv[i], error.c_str());
                failed++;
            }
        }
        return failed == 0 ? 0 : 1;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--preset") == 0) {
        uint64_t maxCycles = 2'000'000ull;
        bool dumpBasic = false;
        std::string wavPath;
        std::string romOverride;
        std::string ce1600pRomOverride;
        std::string saveDir;
        Ce158CliPeer ce158Peer;
        uint64_t runAfter = 0;
        std::string lcdPng;
        std::string lcdTextPath;
        std::string tapeIn;
        std::string tapeOut;
        std::vector<std::pair<uint32_t, uint32_t>> memDumps;
        std::string moduleDir = "Qt6/resources/cards";
        std::vector<std::string> extraModuleDirs;  // 2nd+ `--modules-dir`, searched after `moduleDir`
        bool moduleDirSet = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--dump-basic") == 0) dumpBasic = true;
            else if (std::strcmp(argv[i], "--wav") == 0 && i + 1 < argc) wavPath = argv[++i];
            else if (std::strcmp(argv[i], "--rom") == 0 && i + 1 < argc) romOverride = argv[++i];
            else if (std::strcmp(argv[i], "--ce1600p-rom") == 0 && i + 1 < argc) ce1600pRomOverride = argv[++i];
            else if (std::strcmp(argv[i], "--save-dir") == 0 && i + 1 < argc) saveDir = argv[++i];
            else if (std::strcmp(argv[i], "--run-after") == 0 && i + 1 < argc) runAfter = std::strtoull(argv[++i], nullptr, 10);
            else if (std::strcmp(argv[i], "--lcd-png") == 0 && i + 1 < argc) lcdPng = argv[++i];
            else if (std::strcmp(argv[i], "--lcd-text") == 0 && i + 1 < argc) lcdTextPath = argv[++i];
            else if (std::strcmp(argv[i], "--tape-in") == 0 && i + 1 < argc) tapeIn = argv[++i];
            else if (std::strcmp(argv[i], "--tape-out") == 0 && i + 1 < argc) tapeOut = argv[++i];
            else if (std::strcmp(argv[i], "--dump-mem") == 0 && i + 1 < argc) {
                char* rest = nullptr;
                const uint32_t start = static_cast<uint32_t>(std::strtoul(argv[++i], &rest, 0));
                const uint32_t length = (rest && *rest == ',') ? static_cast<uint32_t>(std::strtoul(rest + 1, nullptr, 0)) : 0;
                if (length == 0) {
                    std::fprintf(stderr, "--dump-mem wants <addr>,<len>\n");
                    return 1;
                }
                memDumps.emplace_back(start, length);
            }
            else if (ce158Peer.parseArg(argc, argv, i)) {}
            else if (std::strcmp(argv[i], "--modules-dir") == 0 && i + 1 < argc) {
                if (!moduleDirSet) { moduleDir = argv[++i]; moduleDirSet = true; }
                else               { extraModuleDirs.push_back(argv[++i]); }
            }
            else maxCycles = std::strtoull(argv[i], nullptr, 10);
        }
        return runPreset(argv[2], maxCycles, dumpBasic, moduleDir, extraModuleDirs, wavPath, romOverride,
                         ce1600pRomOverride, saveDir, ce158Peer, runAfter, lcdPng, lcdTextPath, tapeIn, tapeOut, memDumps);
    }
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <romI-0-file> <romII-0-file> [maxCycles]\n", argv[0]);
        std::fprintf(stderr, "       %s --check-preset <preset-file>...\n", argv[0]);
        std::fprintf(stderr, "       %s --preset <preset-file.pc1600> [maxCycles] [--dump-basic] [--wav <out.wav>] [--tape-in <in.wav>] [--tape-out <out.wav>] [--dump-mem <addr>,<len>] [--rom new|old] [--ce1600p-rom new|old] [--lcd-png <out.png>] [--lcd-text <out.txt|->]\n", argv[0]);
        return 1;
    }
    uint64_t maxCycles = 2'000'000ull;
    if (argc >= 4) maxCycles = std::strtoull(argv[3], nullptr, 10);

    std::vector<uint8_t> lower, upper;
    if (!cli::readFileExact(argv[1], 16384, &lower)) {
        std::fprintf(stderr, "failed to read '%s' (expected exactly 16384 bytes)\n", argv[1]);
        return 1;
    }
    if (!cli::readFileExact(argv[2], 16384, &upper)) {
        std::fprintf(stderr, "failed to read '%s' (expected exactly 16384 bytes)\n", argv[2]);
        return 1;
    }

    PC1600Bank bank;
    PC1600Memory mem(bank);
    if (!mem.loadBank0(lower.data(), lower.size(), upper.data(), upper.size())) {
        std::fprintf(stderr, "loadBank0 failed\n");
        return 1;
    }
    SC7852 cpu(mem);
    cpu.reset();
    std::printf("Reset -> PC=0x%04X\n", cpu.pc());

    static std::array<uint64_t, 65536> pcHistogram{};
    uint64_t consumed = 0;
    uint64_t steps = 0;
    while (consumed < maxCycles) {
        uint16_t pcBefore = cpu.pc();
        int c = cpu.step();
        if (c == 0) {
            std::printf("CPU halted (HALT) at PC=0x%04X after %llu cycles with no pending interrupt\n",
                        pcBefore, (unsigned long long)consumed);
            break;
        }
        pcHistogram[pcBefore]++;
        consumed += static_cast<uint64_t>(c);
        steps++;
    }

    std::printf("Ran %llu instructions, %llu cycles\n", (unsigned long long)steps, (unsigned long long)consumed);

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
    if (distinctPCs < 500) {
        std::printf("Signature matches a converged small loop (stable, bounded execution).\n");
    }

    std::printf("Final registers: AF=0x%04X BC=0x%04X DE=0x%04X HL=0x%04X SP=0x%04X PC=0x%04X IM=%d IFF1=%d\n",
                cpu.af(), cpu.bc(), cpu.de(), cpu.hl(), cpu.sp(), cpu.pc(), cpu.im(), cpu.iff1());
    return 0;
}
