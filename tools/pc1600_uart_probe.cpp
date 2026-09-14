// Throw-away probe: trace the ROM path when BASIC runs an S2: RAM-disk
// operation on a CE-1601M (see docs/PC1600-Core-Limitations.md's
// bank-switching section for the required INIT order of operations).
//
// Boots the full dual-CPU PC1600Machine with a CE-1601M in Slot 2, runs to
// idle, then types a BASIC line (default: INIT "S2:","M"), presses ENTER
// INSIDE the recording window, and reports: every IN/OUT to 20-27H + 33H and
// every sub-CPU command byte with the PC of each access; the LCD rendered as
// ASCII art; the IOCS work area; and every visited (cpu, bank, PC).
//
// An EMPTY line is a CONTROL run -- type nothing, idle for the same window.
// Diffing the visited-PC sets is what separates real work from housekeeping:
//   PC1600_PCSET=ctrl  ./pc1600_uart_probe roms ''
//   PC1600_PCSET=init  ./pc1600_uart_probe roms 'INIT "S2:","M"'
//   PC1600_PCSET=print ./pc1600_uart_probe roms 'PRINT 1+1'
//   comm -13 ctrl init > init_only ; comm -13 ctrl print > print_only
//   comm -13 print_only init_only          # => INIT/S2:-specific code
// (That differential is how the "UART handshake blocks the RAM disk" theory
// was disproved: an INIT run and an idle run visit the SAME addresses.)
//
// Build: tools/build_pc1600_uart_probe.sh
// Run:   ./pc1600_uart_probe [roms-dir] ['BASIC LINE']
//        PC1600_PCSET=<file> to dump every visited (cpu, bank, PC)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../Core/CPU/SC7852/SC7852.hpp"
#include "../Core/Connector/MemoryCardDefinition.hpp"
#include "../Core/Connector/SoftwareDefinedCard.hpp"
#include "../Core/PC1600/PC1600BasicTyper.hpp"
#include "../Core/PC1600/PC1600Machine.hpp"
#include "../Core/PC1600/PC1600PowerProbe.hpp"
#include "../Core/PC1600/PtySerialLink.hpp"

namespace {

bool readRom(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    *out = std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    return out->size() == 16384;
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc >= 2) ? std::string(argv[1]) + "/" : "roms/";
    const std::string line = (argc >= 3) ? argv[2] : "INIT \"S2:\",\"M\"";

    std::vector<uint8_t> i0, ii0, iii3, r3b, iv6, r1500;
    if (!(readRom(dir + "PC1600-P0-B0.bin", &i0) &&
          readRom(dir + "PC1600-P1-B0.bin", &ii0) &&
          readRom(dir + "PC1600-P1-B3.bin", &iii3) &&
          readRom(dir + "PC1600-P1-B3B.bin", &r3b) &&
          readRom(dir + "PC1600-P2-B6.bin", &iv6) &&
          readRom(dir + "PC1600-LH5803-C000-FFFF.bin", &r1500))) {
        std::fprintf(stderr, "could not read the PC1600-*.bin set from '%s'\n", dir.c_str());
        return 1;
    }

    PC1600Machine m;
    if (!m.loadBank0(i0.data(), i0.size(), ii0.data(), ii0.size()) ||
        !m.loadBank3Rom(iii3.data(), iii3.size()) ||
        !m.loadBank3bRom(r3b.data(), r3b.size()) ||
        !m.loadBank6Rom(iv6.data(), iv6.size()) ||
        !m.loadLH5803Rom(r1500.data(), r1500.size())) {
        std::fprintf(stderr, "ROM load failed\n");
        return 1;
    }

    // CE-1601M into Slot 2 (the RAM disk lives here).
    std::string err;
    auto card = makeSoftwareDefinedCard("Calc-U-1600/Resources/ce1601m.card.yaml",
                                        CardHost::PC1600Slot2, &err);
    if (!card) {
        std::fprintf(stderr, "CE-1601M card build failed: %s\n", err.c_str());
        return 1;
    }
    m.attachSlot2Card(std::move(card));

    m.allReset();
    m.seedClock(2026, 9, 2, 12, 0, 0);

    // PC1600_SERIAL=1 attaches a host pseudo-terminal to the TC8576F so a
    // Mac serial app (SharpDataExchange, screen, cat) can exchange bytes
    // with the emulated RS-232C port. After the normal typed-line run the
    // probe then holds ~PC1600_SERIAL_SECONDS (default 180) of real time
    // so the transfer can happen. Drive the emulated side with e.g.
    //   SETCOM"COM1:",9600,8,N,1,X,N : INIT"COM1:",4096 : SAVE"COM1:"
    std::unique_ptr<PtySerialLink> serial;
    if (std::getenv("PC1600_SERIAL")) {
        serial = std::make_unique<PtySerialLink>();
        if (!serial->isOpen()) {
            std::fprintf(stderr, "PC1600_SERIAL: %s\n", serial->lastError().c_str());
            return 1;
        }
        m.setSerialLink(serial.get());
        std::fprintf(stderr,
                     "\n*** serial port ready ***\n  slave : %s\n  stable: %s\n\n",
                     serial->slavePath().c_str(),
                     serial->stablePath().empty() ? "(none)" : serial->stablePath().c_str());
    }

    uint64_t tstates = 0, steps = 0;
    auto stepOnce = [&]() {
        const bool z80 = m.sc7852Owns();
        pc1600probe::g_ownsSc7852 = z80;
        pc1600probe::g_pc = z80 ? m.sc7852().pc() : static_cast<uint16_t>(m.lh5803().pc());
        const int c = m.step();
        const uint64_t cyc = static_cast<uint64_t>(c > 0 ? c : SC7852::kHaltTickCycles);
        tstates += PC1600Machine::toTStates(cyc, z80);
        steps++;
    };

    // Render the dot-matrix as ASCII art -- the only unambiguous way to tell
    // whether typed characters actually reached BASIC, and whether the command
    // printed an error or just returned to a fresh '>' prompt.
    auto dumpDisplay = [&](const char* when) {
        auto s = m.displaySnapshot();
        int lit = 0;
        for (int y = 0; y < PC1600Display::kHeight; y++)
            for (int x = 0; x < PC1600Display::kWidth; x++)
                if (s.pixels[y][x]) lit++;
        std::fprintf(stderr, "\n--- display (%s): %d lit pixels ---\n", when, lit);
        for (int y = 0; y < PC1600Display::kHeight; y++) {
            std::fprintf(stderr, "  |");
            for (int x = 0; x < PC1600Display::kWidth; x++)
                std::fputc(s.pixels[y][x] ? '#' : '.', stderr);
            std::fprintf(stderr, "|\n");
        }
    };

    auto dumpWorkArea = [&](const char* when) {
        std::fprintf(stderr, "\n--- IOCS work area (%s) ---\n", when);
        std::fprintf(stderr, "  F05D = %02X   (b0=%d b1=%d b2=%d b4=%d)\n",
                     m.memory().peek(0xF05D),
                     (m.memory().peek(0xF05D) >> 0) & 1, (m.memory().peek(0xF05D) >> 1) & 1,
                     (m.memory().peek(0xF05D) >> 2) & 1, (m.memory().peek(0xF05D) >> 4) & 1);
        // NB: F083 is the last translated KEY CHARACTER (romIII-3 4900 is the
        // key-code->character translator), not a resolved device or sub-CPU
        // command -- 22h after typing a line ending in '"', 0Dh after ENTER.
        // F05D is a keyboard mode flags byte, likewise not device state.
        std::fprintf(stderr, "  F083 = %02X   (last translated key character)\n",
                     m.memory().peek(0xF083));
        std::fprintf(stderr, "  F084..F08C =");
        for (uint16_t a = 0xF084; a <= 0xF08C; a++)
            std::fprintf(stderr, " %02X", m.memory().peek(a));
        std::fprintf(stderr, "\n");
    };

    // PC1600_WATCH_EARLY=1 arms the probe for the BOOT window too -- only
    // useful together with a narrow PC1600_WATCH range (e.g. the per-slot
    // module descriptors), or the log is unusable.
    if (std::getenv("PC1600_WATCH_EARLY")) {
        unsigned lo = 0, hi = 0;
        if (const char* w = std::getenv("PC1600_WATCH"))
            if (std::sscanf(w, "%x:%x", &lo, &hi) == 2) {
                pc1600probe::g_slotWatchLo = (uint16_t)lo;
                pc1600probe::g_slotWatchHi = (uint16_t)hi;
            }
        pc1600probe::g_slotWatch = true;
        pc1600probe::g_writeWatchAll = false;
        pc1600probe::g_cardWritesOnly = false;
        pc1600probe::g_verbose = false;
        pc1600probe::g_active = true;
    }
    const uint64_t kSettle = static_cast<uint64_t>(PC1600Machine::kTStateHz) * 4;
    while (tstates < kSettle) stepOnce();
    std::fprintf(stderr, "boot settle: %llu steps, %llu T-states\n",
                 (unsigned long long)steps, (unsigned long long)tstates);
    dumpWorkArea("after boot settle");
    dumpDisplay("after boot settle");

    // ── Type the BASIC line, tracing the whole time ────────────────────
    std::fprintf(stderr, "\n=== typing: %s ===\n", line.c_str());
    pc1600probe::g_active = true;
    pc1600probe::g_verbose = true;

    // Split on ';' so a run can submit several lines, e.g.
    //   'INIT "S2:","M";MEM'   -- do the thing, then ask what changed.
    std::vector<std::string> lines;
    for (size_t i = 0, j; i <= line.size(); i = j + 1) {
        j = line.find(';', i);
        if (j == std::string::npos) j = line.size();
        if (j > i) lines.push_back(line.substr(i, j - i));
    }
    if (lines.empty())
        std::fprintf(stderr, "(control run: nothing typed, idling only)\n");

    // ENTER is pressed INSIDE the recording window below, not before it: the
    // command runs and finishes within a few frames of the keypress, so a
    // window that opens afterwards captures nothing but the idle loop (that is
    // exactly why an earlier pass measured an INIT run whose visited-PC set was
    // byte-identical to a do-nothing control run).
    const uint64_t kFrame = PC1600Machine::kTStateHz / 60;
    const uint64_t kRun = static_cast<uint64_t>(PC1600Machine::kTStateHz) * 30;
    const uint64_t tStart = tstates;
    // Hold by ELAPSED TIME, not step count: the ROM samples the keyboard off
    // the 1/64 s tick, so the press has to span several frames.
    (void)kFrame;
    // Watch every page-C read of the ROM's medium-probe buffer (IX=8000h):
    // which branch of PC1600Memory::read() serves it answers "does the probe
    // reach the CE-1601M, or is it reading ROM / open bus?"
    pc1600probe::g_slotWatch = true;
    pc1600probe::g_slotWatchLo = 0x8000;
    pc1600probe::g_slotWatchHi = 0x800F;
    pc1600probe::g_writeWatchAll = true;   // any address, not just the probe window
    pc1600probe::g_cardWritesOnly = true;  // ...but only writes a slot card TOOK
    // PC1600_WATCH=lo:hi narrows the window and reports EVERY access in it,
    // card or not -- e.g. F015:F02A for the per-slot module descriptors.
    if (const char* w = std::getenv("PC1600_WATCH")) {
        unsigned lo = 0, hi = 0;
        if (std::sscanf(w, "%x:%x", &lo, &hi) == 2) {
            pc1600probe::g_slotWatchLo = (uint16_t)lo;
            pc1600probe::g_slotWatchHi = (uint16_t)hi;
            pc1600probe::g_writeWatchAll = false;
            pc1600probe::g_cardWritesOnly = false;
        }
    }
    // Submit each line from INSIDE the recording window: type its characters,
    // then hold ENTER by ELAPSED TIME (the ROM samples the keyboard off the
    // 1/64 s tick, so a step-counted hold is never seen), then let the command
    // run. Driven as a state machine off stepOnce() so every instruction the
    // command executes is recorded -- a window opened after the keypress
    // catches nothing, since a command finishes within a few frames of it.
    size_t nextLine = 0;
    int phase = 0;            // 0=type, 1=hold ENTER, 2=let it run
    uint64_t phaseUntil = 0;
    auto pump = [&]() {
        if (nextLine >= lines.size()) return;
        if (tstates < phaseUntil) return;
        if (phase == 0) {
            // PC1600_POKE=addr:val[,addr:val...] applied just before each line
            // is submitted -- for testing a causal claim about a work-area
            // byte (e.g. the per-slot module descriptor at F020) without
            // guessing at the ROM code that should have produced it.
            if (const char* pk = std::getenv("PC1600_POKE")) {
                unsigned a = 0, v = 0;
                const char* q = pk;
                while (std::sscanf(q, "%x:%x", &a, &v) == 2) {
                    m.memory().poke((uint16_t)a, (uint8_t)v);
                    std::fprintf(stderr, "  [POKE] %04X <- %02X\n", a, v);
                    const char* c = std::strchr(q, ',');
                    if (!c) break;
                    q = c + 1;
                }
            }
            std::string e;
            if (!typeLine(m, lines[nextLine], /*pressEnter=*/false, &e))
                std::fprintf(stderr, "typeLine failed: %s\n", e.c_str());
            std::fprintf(stderr, "\n=== submitting: %s ===\n", lines[nextLine].c_str());
            m.pressKey("enter");
            phase = 1;
            phaseUntil = tstates + kFrame * 4;
        } else if (phase == 1) {
            m.releaseKey("enter");
            phase = 2;
            phaseUntil = tstates + PC1600Machine::kTStateHz * 2; // 2 s to run
        } else {
            dumpDisplay(("after: " + lines[nextLine]).c_str());
            dumpWorkArea(("after: " + lines[nextLine]).c_str());
            nextLine++;
            phase = 0;
        }
    };
    pump();

    std::unordered_map<uint32_t, uint64_t> pcHist;
    std::unordered_map<uint32_t, uint64_t> pcFirst; // key -> step index first seen
    std::vector<uint32_t> pcOrder;                  // "interesting" PCs, first-seen order
    auto interesting = [](uint16_t pc) {
        // Known-noise regions from earlier passes: idle HALT loop, the 1/64s
        // timer ISR, the RST/BANKJP trampoline, and the TC8576F + sub-CPU
        // readiness handshake itself. Everything else is INIT dispatch logic.
        if (pc >= 0x9280 && pc <= 0x92FF) return false; // idle HALT
        if (pc >= 0x4090 && pc <= 0x4200) return false; // timer ISR body
        if (pc >= 0x0800 && pc <= 0x08FF) return false; // bank trampoline
        if (pc >= 0x42C0 && pc <= 0x42D0) return false; // ISR tail
        if (pc >= 0xA870 && pc <= 0xAB20) return false; // UART / sub-CPU handshake helpers
        if (pc >= 0x4860 && pc <= 0x48B0) return false; // 1Cxx port housekeeping seen in ISR
        return true;
    };
    while (tstates - tStart < kRun) {
        const bool z80 = m.sc7852Owns();
        const uint16_t pc = z80 ? m.sc7852().pc() : static_cast<uint16_t>(m.lh5803().pc());
        // Tag the PC with the bank actually mapped under it: page B (4000-7FFF)
        // can be romIII-3, the hidden rom3b, or RAM, and page C (8000-BFFF) is
        // bank-selected too -- a bare 16-bit PC is ambiguous across all three.
        uint32_t bankTag = 0;
        if (pc >= 0x4000 && pc <= 0x7FFF)
            bankTag = (uint32_t)m.bank().pageBBank() |
                      (m.bank().hiddenBasicRomSelected() ? 0x08u : 0u);
        else if (pc >= 0x8000 && pc <= 0xBFFF)
            bankTag = 0x10u | m.bank().pageCBank();
        else if (pc >= 0x0000 && pc <= 0x3FFF)
            bankTag = 0x20u | m.bank().pageABank();
        const uint32_t key = (z80 ? 0u : 0x10000u) | (bankTag << 20) | pc;
        if (pcHist.find(key) == pcHist.end()) {
            pcFirst[key] = steps;
            if (z80 && interesting(pc)) pcOrder.push_back(key);
        }
        pcHist[key]++;
        stepOnce();
        pump();
        if (tstates - tStart >= kRun) break;
    }

    // Every visited PC, sorted -- so an INIT run can be diffed against a
    // control (empty-line) run and leave only the code INIT itself reaches.
    if (const char* path = std::getenv("PC1600_PCSET")) {
        if (FILE* f = std::fopen(path, "w")) {
            std::vector<uint32_t> all;
            for (auto& kv : pcHist) all.push_back(kv.first);
            std::sort(all.begin(), all.end());
            for (uint32_t k : all)
                std::fprintf(f, "%s b%02X %04X %llu\n", (k & 0x10000) ? "5803" : "Z80",
                             (k >> 20) & 0xFF, k & 0xFFFF,
                             (unsigned long long)pcFirst[k]);
            std::fclose(f);
            std::fprintf(stderr, "\nwrote %zu visited PCs to %s\n", all.size(), path);
        }
    }

    std::fprintf(stderr, "\n=== interesting PCs, first-seen order (%zu) ===\n", pcOrder.size());
    for (uint32_t key : pcOrder)
        std::fprintf(stderr, "  %04X  first@step %llu  x%llu\n", key & 0xFFFF,
                     (unsigned long long)pcFirst[key], (unsigned long long)pcHist[key]);

    dumpWorkArea("after INIT gave up");
    dumpDisplay("at end of run");

    std::fprintf(stderr, "\n=== sub-CPU command tally (after type) ===\n");
    for (int i = 0; i < 256; i++)
        if (pc1600probe::g_subCmdCount[i])
            std::fprintf(stderr, "  cmd %02X : x%llu\n", i,
                         (unsigned long long)pc1600probe::g_subCmdCount[i]);
    std::fprintf(stderr, "\n=== IO write tally (ports 20-3F) ===\n");
    for (int p = 0x20; p <= 0x3F; p++)
        if (pc1600probe::g_ioWriteCount[p])
            std::fprintf(stderr, "  OUT %02X : x%llu\n", p,
                         (unsigned long long)pc1600probe::g_ioWriteCount[p]);

    std::fprintf(stderr, "\n=== post-command PC profile (top 20 of %zu) ===\n", pcHist.size());
    std::vector<std::pair<uint32_t, uint64_t>> v(pcHist.begin(), pcHist.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < v.size() && i < 20; i++)
        std::fprintf(stderr, "  %s %04X  x%llu\n", (v[i].first & 0x10000) ? "5803" : "Z80 ",
                     v[i].first & 0xFFFF, (unsigned long long)v[i].second);


    // ── Did INIT format vbank 1? (boot sector byte 0 == 0x55) ──────────
    {
        auto img = m.debugSlotImage(2);
        std::fprintf(stderr, "\n=== Slot 2 image: %zu bytes ===\n", img.size());
        for (size_t base = 0; base + 16 <= img.size(); base += 0x4000) {
            std::fprintf(stderr, "  @%06zX:", base);
            for (int i = 0; i < 16; i++) std::fprintf(stderr, " %02X", img[base + i]);
            std::fprintf(stderr, "\n");
            if (base >= 0x1'0000) break;
        }
    }

    // ── Serial hold: keep stepping ~real-time so a host transfer can run ──
    if (serial) {
        const char* s = std::getenv("PC1600_SERIAL_SECONDS");
        const double secs = s ? std::atof(s) : 180.0;
        const std::string& port = serial->preferredPath();
        std::fprintf(stderr,
                     "\n=== serial hold: ~%.0fs real-time for a transfer ===\n"
                     "  e.g.  java -jar SharpDataExchange.jar get -p %s\n",
                     secs, port.c_str());
        const auto t0 = std::chrono::steady_clock::now();
        const std::chrono::duration<double> budget(secs);
        while (std::chrono::steady_clock::now() - t0 < budget) {
            const uint64_t before = tstates;
            while (tstates - before < PC1600Machine::kTStateHz / 100) stepOnce();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        m.setSerialLink(nullptr);
    }

    return 0;
}
