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
//
// CE-158 (a preset with `interface: ce158`):
//   --ce158-pty       attach a host PTY as the RS-232C peer; its stable
//                     symlink is ~/Library/Application Support/Calc-U-1600/
//                     calcu1600-ce158.serial (path printed on stderr).
//   --ce158-rx <file> scripted peer: the file's bytes are what the CE-158
//                     receives, in order.
//   --ce158-rx-hold <n> start sending the --ce158-rx bytes only after n
//                     character times (the ROM flushes the receiver when
//                     e.g. SETDEV runs, so a byte sent too early is lost).
//   --ce158-tx <file> write every byte the CE-158 sends to <file>.
// Anything printed on the Centronics port is dumped after the run.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../Core/Audio/WavFile.hpp"
#include "../Core/PC1500/PC1500Machine.hpp"
#include "../Core/PC1500/PresetFile.hpp"
#include "../Core/PC1500/PC1500PresetLoader.hpp"
#include "../Core/Serial/PtySerialLink.hpp"

namespace {

// --ce158-rx / --ce158-tx: a file-backed serial peer for repeatable runs.
class FileSerialLink final : public SerialLink {
public:
    std::vector<uint8_t> rx;
    size_t rxPos = 0;
    std::vector<uint8_t> tx;
    long holdPolls = 0; // --ce158-rx-hold: character times to stay silent first
    bool poll(uint8_t& out) override {
        if (holdPolls > 0) { --holdPolls; return false; }
        if (rxPos >= rx.size()) return false;
        out = rx[rxPos++];
        return true;
    }
    void send(uint8_t byte) override { tx.push_back(byte); }
};

void printBytes(const char* title, const std::vector<uint8_t>& bytes) {
    std::printf("%s (%zu bytes):\n", title, bytes.size());
    std::string text;
    for (uint8_t b : bytes) {
        if (b == '\r') continue;
        if (b == '\n' || (b >= 0x20 && b < 0x7F)) text += static_cast<char>(b);
        else { char hex[8]; std::snprintf(hex, sizeof hex, "<%02X>", b); text += hex; }
    }
    std::printf("%s\n", text.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    // Pull an optional `--modules-dir <dir>` out of argv up front so the
    // rest of the parsing keeps its simple fixed positions.
    std::string moduleDir = "Calc-U-1600/Resources";
    std::vector<std::string> extraModuleDirs;  // 2nd+ `--modules-dir`, searched after `moduleDir`
    bool moduleDirSet = false;
    bool dumpBasic = false;
    std::string wavPath;
    bool ce158Pty = false;
    std::string ce158RxPath, ce158TxPath;
    long ce158RxHold = 0;
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
            if (std::strcmp(argv[i], "--ce158-pty") == 0) { ce158Pty = true; continue; }
            if (std::strcmp(argv[i], "--ce158-rx") == 0 && i + 1 < argc) { ce158RxPath = argv[++i]; continue; }
            if (std::strcmp(argv[i], "--ce158-rx-hold") == 0 && i + 1 < argc) { ce158RxHold = std::strtol(argv[++i], nullptr, 10); continue; }
            if (std::strcmp(argv[i], "--ce158-tx") == 0 && i + 1 < argc) { ce158TxPath = argv[++i]; continue; }
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
        std::fprintf(stderr, "                --ce158-pty | --ce158-rx <file> [--ce158-rx-hold <n>] --ce158-tx <file>\n");
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

    // The CE-158's serial peer. Set before the preset attaches the card --
    // attachCE158() picks up whatever link the machine already holds.
    std::unique_ptr<PtySerialLink> ptyLink;
    FileSerialLink fileLink;
    if (ce158Pty) {
        ptyLink = std::make_unique<PtySerialLink>(std::string{}, PtySerialLink::kCE158LinkName);
        if (!ptyLink->isOpen()) {
            std::fprintf(stderr, "CE-158 PTY: %s\n", ptyLink->lastError().c_str());
            return 1;
        }
        std::fprintf(stderr, "CE-158 serial port: %s\n", ptyLink->preferredPath().c_str());
        machine.setCE158SerialLink(ptyLink.get());
    } else if (!ce158RxPath.empty() || !ce158TxPath.empty()) {
        if (!ce158RxPath.empty()) {
            std::FILE* f = std::fopen(ce158RxPath.c_str(), "rb");
            if (!f) { std::fprintf(stderr, "cannot read %s\n", ce158RxPath.c_str()); return 1; }
            int ch;
            while ((ch = std::fgetc(f)) != EOF) fileLink.rx.push_back(static_cast<uint8_t>(ch));
            std::fclose(f);
        }
        fileLink.holdPolls = ce158RxHold;
        machine.setCE158SerialLink(&fileLink);
    }

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

    if (machine.ce158Attached()) {
        std::printf("CE-158: attached, baud=%d, UART status=%02X\n", machine.ce158Card()->baudRate(),
                    machine.ce158Card()->uartStatus());
        printBytes("CE-158 Centronics output", machine.drainCE158ParallelOutput());
        if (!ce158RxPath.empty())
            std::printf("CE-158 serial: received %zu of %zu scripted bytes\n", fileLink.rxPos, fileLink.rx.size());
        if (!ce158TxPath.empty() || !ce158RxPath.empty()) printBytes("CE-158 serial output", fileLink.tx);
        if (!ce158TxPath.empty()) {
            std::FILE* f = std::fopen(ce158TxPath.c_str(), "wb");
            if (!f) { std::fprintf(stderr, "cannot write %s\n", ce158TxPath.c_str()); return 1; }
            std::fwrite(fileLink.tx.data(), 1, fileLink.tx.size(), f);
            std::fclose(f);
        }
    }
    machine.setCE158SerialLink(nullptr);

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
