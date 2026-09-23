// Headless CLI harness for the PC-1600's SC7852 (Z-80) core, single-CPU,
// no LH5803/no dual-CPU arbitration. Boots romI-0+romII-0 (given on the command line), runs for a fixed cycle budget,
// and reports a PC-visit histogram used to detect convergence onto a
// small repeating loop -- mirrors pc1500_cli.cpp's own convergence-signal
// approach (see that file's comment), minus the preset/trace-ring support
// it has, since neither exists for SC7852 yet (trace lands in Phase 5.3).
//
// Usage: pc1600_cli <romI-0-file> <romII-0-file> [maxCycles]
//        pc1600_cli --preset <preset-file.pc1600> [maxCycles] [--dump-basic] [--modules-dir <dir>] [--save-dir <dir>] [--wav <out.wav>] [--rom new|old] [--ce1600p-rom new|old]
//
// The --preset form loads the confirmed PC-1600 ROM set from roms/ (same
// names pc1600_preset_tests.cpp uses), builds a full PC1600Machine, and
// applies a `.pc1600` scenario via applyPC1600Preset() -- the SC7852-only
// histogram mode below is skipped. --dump-basic then prints the BASIC
// program pointers and the raw program-area bytes (read-only), the oracle
// for the fast BASIC loader work (see
// ~/.claude/plans/goal-faster-basic-program-woolly-wall.md).
//
// --rom new|old (--preset only) overrides the preset's PC-1600 ROM version
// (`model: PC-1600:new|old`, default new).
// --ce1600p-rom new|old (--preset only) overrides the preset's CE-1600P ROM
// version (`plotter: ce1600p:new|old`, default new); independent of --rom.
//
// --save-dir <dir> (--preset only) makes a `- saveas: floppy:<name>` step
// write the live disk to <dir>/<name>.floppy.yaml (without it, saveas is a
// logged no-op). Card saveas targets are not supported here.
//
// --wav <out.wav> (--preset only) records the buzzer (OPC 18H, see
// PiezoSampler.hpp) while the preset script runs, as 48 kHz mono 16-bit
// PCM -- as the host hears it, i.e. through the PC-1600 transducer model.

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
#include "../Core/PC1500/PresetFile.hpp"
#include "../Core/Resources/BundledRomCatalog.hpp"

namespace {
bool readFile(const std::string& path, std::vector<uint8_t>* out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out->resize(16384);
    size_t n = std::fread(out->data(), 1, 16384, f);
    std::fclose(f);
    return n == 16384;
}

int runPreset(const std::string& presetPath, uint64_t maxCycles, bool dumpBasic,
              const std::string& moduleDir, const std::vector<std::string>& extraModuleDirs,
              const std::string& wavPath, const std::string& romOverride,
              const std::string& ce1600pRomOverride, const std::string& saveDir) {
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
    PC1600PresetSaveAsFn onSaveAs;
    if (!saveDir.empty()) {
        onSaveAs = [&machine, &saveDir](PresetStep::SaveAsTarget target, const std::string& name,
                                        std::string* err) {
            if (target != PresetStep::SaveAsTarget::Floppy) {
                *err = "pc1600_cli --save-dir only saves floppies";
                return false;
            }
            const std::string path = saveDir + "/" + name + kFloppyFileSuffix;
            FILE* f = std::fopen(path.c_str(), "wb");
            if (!f) {
                *err = "cannot write " + path;
                return false;
            }
            const std::string text = formatFloppyFile(name, machine.ce1600fDiskImage());
            const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
            return std::fclose(f) == 0 && ok;
        };
    }
    PC1600PresetLoadResult loaded = applyPC1600Preset(
        machine, preset,
        [](const std::string& line) { std::fprintf(stderr, "[preset] %s\n", line.c_str()); }, ".",
        moduleDir,
        /*onBooted=*/{}, /*romDirs=*/{"roms"}, extraModuleDirs, /*onArmed=*/{}, onSaveAs);
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
    if (argc >= 3 && std::strcmp(argv[1], "--preset") == 0) {
        uint64_t maxCycles = 2'000'000ull;
        bool dumpBasic = false;
        std::string wavPath;
        std::string romOverride;
        std::string ce1600pRomOverride;
        std::string saveDir;
        std::string moduleDir = ".";
        std::vector<std::string> extraModuleDirs;  // 2nd+ `--modules-dir`, searched after `moduleDir`
        bool moduleDirSet = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--dump-basic") == 0) dumpBasic = true;
            else if (std::strcmp(argv[i], "--wav") == 0 && i + 1 < argc) wavPath = argv[++i];
            else if (std::strcmp(argv[i], "--rom") == 0 && i + 1 < argc) romOverride = argv[++i];
            else if (std::strcmp(argv[i], "--ce1600p-rom") == 0 && i + 1 < argc) ce1600pRomOverride = argv[++i];
            else if (std::strcmp(argv[i], "--save-dir") == 0 && i + 1 < argc) saveDir = argv[++i];
            else if (std::strcmp(argv[i], "--modules-dir") == 0 && i + 1 < argc) {
                if (!moduleDirSet) { moduleDir = argv[++i]; moduleDirSet = true; }
                else               { extraModuleDirs.push_back(argv[++i]); }
            }
            else maxCycles = std::strtoull(argv[i], nullptr, 10);
        }
        return runPreset(argv[2], maxCycles, dumpBasic, moduleDir, extraModuleDirs, wavPath, romOverride,
                         ce1600pRomOverride, saveDir);
    }
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <romI-0-file> <romII-0-file> [maxCycles]\n", argv[0]);
        std::fprintf(stderr, "       %s --preset <preset-file.pc1600> [maxCycles] [--dump-basic] [--wav <out.wav>] [--rom new|old] [--ce1600p-rom new|old]\n", argv[0]);
        return 1;
    }
    uint64_t maxCycles = 2'000'000ull;
    if (argc >= 4) maxCycles = std::strtoull(argv[3], nullptr, 10);

    std::vector<uint8_t> lower, upper;
    if (!readFile(argv[1], &lower)) {
        std::fprintf(stderr, "failed to read '%s' (expected exactly 16384 bytes)\n", argv[1]);
        return 1;
    }
    if (!readFile(argv[2], &upper)) {
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
