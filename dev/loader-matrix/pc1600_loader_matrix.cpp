// One-time verification (dev/loader-matrix/README.md), PC-1600 part: loads
// one program twice -- through the ROM over COM1: (`LOAD`/`BLOAD "COM1:"`)
// or over the CE-158 (`CLOAD`/`CLOAD M`, MODE 1 only), fed the bytes `sde
// put` sends, and through the emulator's own loader (a preset's `program:
// file:`) -- and compares the memory afterwards. The CE-158 is attached in
// every run.
//
//   pc1600_loader_matrix --probe --mode <0|1> [--slot1 <card>] [--slot2 <card>]
//   pc1600_loader_matrix --mode <0|1> [--slot1 <card>] [--slot2 <card>]
//                        --transport <com1|ce158> --kind <basic|ml>
//                        --stream <sde file> --loader-file <file> [--expect-refuse]
//
// --stream is the `sde convert` output -- what `sde put` writes: the header
// (16 bytes PC-1600, 27 bytes CE-158) at full speed, a 300 ms pause, then
// the payload a byte at a time with 1 ms between bytes.
//
// Build: dev/loader-matrix/build_pc1600.sh. Run from the repo root.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "../../Core/PC1600/PC1600LcdText.hpp"
#include "../../Core/PC1600/PC1600Machine.hpp"
#include "../../Core/PC1600/PC1600PresetLoader.hpp"
#include "../../Core/Preset/PresetFile.hpp"
#include "../../Core/Resources/BundledRomCatalog.hpp"
#include "../../Core/Serial/SerialLink.hpp"

namespace {

constexpr const char* kScratchDir = "headless/loader-matrix";

// Work-area bytes that hold where the program is and how far it goes. They
// must match after both loads (PC-1600-Work-Area-Map.md).
struct Span { uint16_t from, to; const char* name; };
const Span kPointers[] = {
    {0xF016, 0xF023, "S1MTb/S1MBb/S2MTb/S2MBb descriptors"},
    {0xF02A, 0xF02C, "S0MTb, BASPRG_ST/END banks"},
    {0xF1C1, 0xF1C1, "CURRENT_TOP bank"},
    {0xF1D5, 0xF1DA, "TITLE, ADTBL"},
    {0xF865, 0xF86A, "BASPRG_ST/END/EDT"},
    {0xF899, 0xF89A, "VARIABLE_PTR"},
    {0xF89D, 0xF89E, "F89D / CURRENT_TOP"},
    {0xFE3C, 0xFE41, "PRGADR"},
};

// What `sde put` does: header, HEADER_PAUSE (300 ms), then BYTE_DELAY (1 ms)
// after each payload byte. poll() is called once per character time.
class SdeLink final : public SerialLink {
public:
    std::vector<uint8_t> rx;
    size_t headerLen = 0;
    size_t pos = 0;
    long pausePolls = 0;   // the header pause, in character times
    long gapPolls = 0;     // idle character times after each payload byte
    long gapLeft = 0;
    bool armed = false;
    bool poll(uint8_t& out) override {
        if (!armed || pos >= rx.size()) return false;
        if (pos == headerLen && pausePolls > 0) { --pausePolls; return false; }
        if (gapLeft > 0) { --gapLeft; return false; }
        out = rx[pos++];
        if (pos > headerLen) gapLeft = gapPolls;
        return true;
    }
    void send(uint8_t) override {}
    bool done() const { return pos >= rx.size(); }
};

struct Config {
    int mode = 0;
    std::string slot1, slot2;
    bool com1 = true;
    bool ml = false;
};

bool readFile(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

std::string lcd(PC1600Machine& m) {
    std::string t = pc1600LcdText(m).plainText();
    for (char& c : t) if (c == '\n') c = '|';
    return t;
}

struct Snapshot {
    std::vector<uint8_t> ram;          // Z-80 $C000-$FFFF (LH5803 $4000-$7FFF)
    std::vector<uint8_t> slot1, slot2; // the cards' backing stores
};

Snapshot snapshot(PC1600Machine& m) {
    Snapshot s;
    s.ram.resize(0x4000);
    for (uint32_t a = 0; a < 0x4000; ++a) s.ram[a] = m.memory().peek(uint16_t(0xC000 + a));
    s.slot1 = m.memory().slot1CardImage();
    s.slot2 = m.memory().slot2CardImage();
    return s;
}

std::string absPath(const std::string& p) {
    char buf[4096];
    return (!p.empty() && p[0] != '/' && realpath(p.c_str(), buf)) ? std::string(buf) : p;
}

// Builds and applies the preset: CE-158, the slot cards, MODE, PRO mode,
// NEW0, then `extraKeys`, then `program: file:` if `loaderFile` is set.
std::unique_ptr<PC1600Machine> applyPreset(const Config& c, SerialLink* com1, SerialLink* ce158,
                                           const std::vector<std::string>& extraKeys,
                                           const std::string& loaderFile, PresetLoadResult* res) {
    std::string yaml = "format-version: 1\nmodel: PC-1600\ninterface: CE-158\n";
    if (!c.slot1.empty()) yaml += "slot-1: " + c.slot1 + "\n";
    if (!c.slot2.empty()) yaml += "slot-2: " + c.slot2 + "\n";
    yaml += "keys:\n";
    if (c.mode == 1) yaml += "  - type: MODE 1\n";
    yaml += "  - key: mode\n  - type: NEW0\n";
    for (const std::string& k : extraKeys) yaml += "  - type: " + k + "\n";
    if (!loaderFile.empty()) yaml += "program:\n  file: " + absPath(loaderFile) + "\n";
    const std::string path = std::string(kScratchDir) + "/run.pc1600";
    std::ofstream(path) << yaml;
    PresetFile preset;
    std::string err;
    if (!parsePresetFile(path, &preset, &err)) {
        res->ok = false;
        res->error = "preset: " + err;
        return nullptr;
    }
    auto m = std::make_unique<PC1600Machine>();
    if (!BundledRoms::loadPC1600RomSet(*m, {"roms"}, preset.romVariant, &err)) {
        res->ok = false;
        res->error = "ROM set: " + err;
        return nullptr;
    }
    m->setSerialLink(com1);
    m->setCE158SerialLink(ce158);
    *res = applyPC1600Preset(*m, preset, {}, ".", "Qt6/resources/cards", {}, {"roms"});
    return m;
}

std::vector<std::string> serialSetup(const Config& c) {
    if (c.com1) return {"SETCOM \"COM1:\",9600,8,N,1,N,N", "RCVSTAT \"COM1:\",28"};
    return {"SETCOM 1200", "SETDEV CI"};
}

struct Run {
    bool ok = false;
    std::string note;
    Snapshot mem;
};

Run runRom(const Config& c, const std::vector<uint8_t>& stream) {
    Run r;
    SdeLink link;
    link.rx = stream;
    link.headerLen = c.com1 ? 16 : 27;
    const int baud = c.com1 ? 9600 : 1200;
    link.pausePolls = long(0.3 * baud / 10 + 0.5);
    link.gapPolls = c.com1 ? 1 : 0; // 1 ms is about one character time at 9600, < one at 1200
    std::vector<std::string> keys = serialSetup(c);
    if (c.com1) keys.push_back(c.ml ? "BLOAD \"COM1:\"" : "LOAD \"COM1:\"");
    else keys.push_back(c.ml ? "CLOAD M" : "CLOAD");
    PresetLoadResult res;
    auto m = applyPreset(c, c.com1 ? &link : nullptr, c.com1 ? nullptr : &link, keys, "", &res);
    if (!m || !res.ok) {
        r.note = "preset failed: " + res.error;
        return r;
    }
    link.armed = true;
    const uint64_t chunk = PC1600Machine::kTStateHz / 10;
    const double secs = 30 + stream.size() * (10.0 / baud) * (1 + link.gapPolls) * 1.5;
    uint64_t budget = uint64_t(secs * PC1600Machine::kTStateHz);
    while (!link.done() && budget > chunk) { m->runCycles(chunk); budget -= chunk; }
    m->runCycles(PC1600Machine::kTStateHz * 2);
    const std::string screen = lcd(*m);
    r.ok = link.done() && screen.find("ERROR") == std::string::npos;
    r.note = (link.done() ? "" : "stream not consumed (" + std::to_string(link.pos) + "/" +
                                 std::to_string(stream.size()) + "); ") + "lcd=[" + screen + "]";
    r.mem = snapshot(*m);
    m->setSerialLink(nullptr);
    m->setCE158SerialLink(nullptr);
    return r;
}

Run runLoader(const Config& c, const std::string& loaderFile) {
    Run r;
    PresetLoadResult res;
    auto m = applyPreset(c, nullptr, nullptr, serialSetup(c), loaderFile, &res);
    if (!m) { r.note = res.error; return r; }
    m->runCycles(PC1600Machine::kTStateHz / 2);
    r.ok = res.ok;
    r.note = (res.ok ? "" : "error: " + res.error + "; ") + "lcd=[" + lcd(*m) + "]";
    r.mem = snapshot(*m);
    return r;
}

// Differing byte ranges of a/b over [from, to), named by `base` + offset.
std::string ranges(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t from, uint32_t to,
                   uint32_t base, size_t* count) {
    std::string out;
    *count = 0;
    if (a.size() != b.size()) { *count = 1; return "size differs"; }
    to = std::min<uint32_t>(to, uint32_t(a.size()));
    for (uint32_t x = from; x < to;) {
        if (a[x] == b[x]) { ++x; continue; }
        uint32_t y = x;
        while (y < to && a[y] != b[y]) ++y;
        *count += y - x;
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s&%04X-&%04X", out.empty() ? "" : ",", base + x, base + y - 1);
        out += buf;
        if (y - x <= 2) {
            for (uint32_t z = x; z < y; ++z) {
                std::snprintf(buf, sizeof buf, "%s%02X/%02X", z == x ? "(" : " ", a[z], b[z]);
                out += buf;
            }
            out += ")";
        }
        x = y;
    }
    return out;
}

uint16_t be16(const std::vector<uint8_t>& ram, uint16_t z80) {
    return uint16_t(ram[z80 - 0xC000] << 8 | ram[z80 - 0xC000 + 1]);
}

int probe(const Config& c) {
    PresetLoadResult res;
    auto m = applyPreset(c, nullptr, nullptr, {"MEM"}, "", &res);
    if (!m || !res.ok) {
        std::fprintf(stderr, "preset failed: %s\n", res.error.c_str());
        return 2;
    }
    const Snapshot s = snapshot(*m);
    std::string screen = lcd(*m);
    long mem = -1;
    for (size_t i = screen.find("MEM"); i != std::string::npos && i < screen.size(); ++i) {
        if (isdigit(static_cast<unsigned char>(screen[i]))) { mem = std::strtol(screen.c_str() + i, nullptr, 10); break; }
    }
    // BASPRG_ST is kept LH5803-side; Z-80 = +$8000.
    std::printf("BMODE=%02X BASPRG_ST=%04X BANK=%02X VARIABLE_PTR=%04X MEM=%ld ADTBL=%02X,%02X,%02X,%02X,%02X\n",
                s.ram[0xF1BC - 0xC000], be16(s.ram, 0xF865), s.ram[0xF02B - 0xC000], be16(s.ram, 0xF899), mem,
                s.ram[0xF1D6 - 0xC000], s.ram[0xF1D7 - 0xC000], s.ram[0xF1D8 - 0xC000], s.ram[0xF1D9 - 0xC000],
                s.ram[0xF1DA - 0xC000]);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Config c;
    std::string transport = "com1", kind, streamPath, loaderFile;
    bool doProbe = false, expectRefuse = false;
    for (int i = 1; i < argc; ++i) {
        auto val = [&](std::string* dst) { if (i + 1 < argc) *dst = argv[++i]; };
        std::string mode;
        if (!std::strcmp(argv[i], "--probe")) doProbe = true;
        else if (!std::strcmp(argv[i], "--expect-refuse")) expectRefuse = true;
        else if (!std::strcmp(argv[i], "--mode")) { val(&mode); c.mode = std::atoi(mode.c_str()); }
        else if (!std::strcmp(argv[i], "--slot1")) val(&c.slot1);
        else if (!std::strcmp(argv[i], "--slot2")) val(&c.slot2);
        else if (!std::strcmp(argv[i], "--transport")) val(&transport);
        else if (!std::strcmp(argv[i], "--kind")) val(&kind);
        else if (!std::strcmp(argv[i], "--stream")) val(&streamPath);
        else if (!std::strcmp(argv[i], "--loader-file")) val(&loaderFile);
        else { std::fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    if (c.slot1 == "none") c.slot1.clear();
    if (c.slot2 == "none") c.slot2.clear();
    c.com1 = transport == "com1";
    c.ml = kind == "ml";
    if (doProbe) return probe(c);

    std::vector<uint8_t> stream;
    const size_t headerLen = c.com1 ? 16 : 27;
    if (!readFile(streamPath, &stream) || stream.size() <= headerLen) {
        std::fprintf(stderr, "cannot read stream %s\n", streamPath.c_str());
        return 2;
    }

    Run a = runRom(c, stream);
    Run b = runLoader(c, loaderFile);
    std::printf("ROM:    %s %s\n", a.ok ? "ok" : "REFUSED/FAILED", a.note.c_str());
    std::printf("LOADER: %s %s\n", b.ok ? "ok" : "REFUSED/FAILED", b.note.c_str());
    if (expectRefuse) {
        const bool pass = !a.ok && !b.ok;
        std::printf("RESULT %s (expect refuse)\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }
    if (a.mem.ram.empty() || b.mem.ram.empty()) {
        std::printf("RESULT FAIL (no memory snapshot)\n");
        return 1;
    }

    size_t nUser = 0, nS1 = 0, nS2 = 0, nPtr = 0, nWork = 0;
    const std::string dUser = ranges(a.mem.ram, b.mem.ram, 0x0000, 0x3000, 0xC000, &nUser);
    const std::string dS1 = ranges(a.mem.slot1, b.mem.slot1, 0, uint32_t(a.mem.slot1.size()), 0, &nS1);
    const std::string dS2 = ranges(a.mem.slot2, b.mem.slot2, 0, uint32_t(a.mem.slot2.size()), 0, &nS2);
    std::string dPtr;
    for (const Span& s : kPointers) {
        size_t n = 0;
        const std::string d = ranges(a.mem.ram, b.mem.ram, s.from - 0xC000, s.to - 0xC000 + 1, 0xC000, &n);
        if (n) { nPtr += n; dPtr += std::string(dPtr.empty() ? "" : ", ") + s.name + " " + d; }
    }
    const std::string dWork = ranges(a.mem.ram, b.mem.ram, 0x3000, 0x4000, 0xC000, &nWork);

    std::printf("BASPRG_ST rom=%04X loader=%04X  BASPRG_END rom=%04X loader=%04X\n", be16(a.mem.ram, 0xF865),
                be16(b.mem.ram, 0xF865), be16(a.mem.ram, 0xF867), be16(b.mem.ram, 0xF867));
    std::printf("internal RAM &C000-&EFFF diff: %zu bytes %s\n", nUser, dUser.c_str());
    std::printf("slot 1 image diff: %zu bytes %s\n", nS1, dS1.c_str());
    std::printf("slot 2 image diff: %zu bytes %s\n", nS2, dS2.c_str());
    std::printf("pointers diff: %zu bytes %s\n", nPtr, dPtr.c_str());
    std::printf("work area &F000-&FFFF diff (info): %zu bytes %s\n", nWork, dWork.c_str());

    const bool pass = a.ok && b.ok && nUser == 0 && nS1 == 0 && nS2 == 0 && nPtr == 0;
    std::printf("RESULT %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
