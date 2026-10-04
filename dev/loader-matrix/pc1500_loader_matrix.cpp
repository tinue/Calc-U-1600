// One-time verification (dev/loader-matrix/README.md): loads one program into
// a PC-1500/1500A twice -- through the ROM (CLOAD / CLOAD M over the CE-158,
// fed the bytes `sde put` sends) and through the emulator's own loader (a
// preset's `program: file:`) -- and compares the memory afterwards. The
// CE-158 is attached in both runs.
//
//   pc1500_loader_matrix --probe --model <PC-1500|PC-1500A> [--card <name>]
//   pc1500_loader_matrix --model <m> [--card <name>] --kind <basic|ml>
//                        --stream <sde file> --loader-file <file> [--expect-refuse]
//
// --stream is the `sde convert` output (CE-158 header + payload), which is
// what `sde put` writes to the wire: header first, a 300 ms pause, then the
// payload. --loader-file is what our loader gets (`.bas` or `.ce158.bin`).
// --expect-refuse: the program doesn't fit; both paths must refuse it.
//
// Build: dev/loader-matrix/build.sh. Run from the repo root.

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

#include "../../Core/PC1500/PC1500LcdText.hpp"
#include "../../Core/PC1500/PC1500Machine.hpp"
#include "../../Core/PC1500/PC1500PresetLoader.hpp"
#include "../../Core/Preset/PresetFile.hpp"
#include "../../Core/Serial/SerialLink.hpp"

namespace {

constexpr size_t kHeaderLen = 27;                 // CE-158 header
constexpr const char* kScratchDir = "headless/loader-matrix";
constexpr uint16_t kRamSt = 0x7863, kRamEnd = 0x7864, kBasPrgSt = 0x7865, kBasPrgEnd = 0x7867;

// What `sde put` does on a PC-1500: header at full speed, HEADER_PAUSE
// (300 ms), then the payload. poll() is called once per character time.
class SdeLink final : public SerialLink {
public:
    std::vector<uint8_t> rx;
    size_t pos = 0;
    long pausePolls = 0;
    bool armed = false;
    bool poll(uint8_t& out) override {
        if (!armed || pos >= rx.size()) return false;
        if (pos == kHeaderLen && pausePolls > 0) { --pausePolls; return false; }
        out = rx[pos++];
        return true;
    }
    void send(uint8_t) override {}
    bool done() const { return pos >= rx.size(); }
};

bool readFile(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

uint16_t peek16(PC1500Machine& m, uint16_t a) { // big-endian, as the ROM keeps them
    return uint16_t(m.memory().peek(a) << 8 | m.memory().peek(uint16_t(a + 1)));
}

std::vector<uint8_t> snapshot(PC1500Machine& m) {
    std::vector<uint8_t> mem(0x8000);
    for (uint32_t a = 0; a < mem.size(); ++a) mem[a] = m.memory().peek(uint16_t(a));
    return mem;
}

std::string lcd(PC1500Machine& m) {
    std::string t = pc1500LcdText(m).plainText();
    for (char& c : t) if (c == '\n') c = '|';
    return t;
}

struct Run {
    bool ok = false;          // the load succeeded
    std::string note;         // error / LCD text
    std::vector<uint8_t> mem;
};

// Builds and applies a preset. `extraKeys` are typed after NEW0; `loaderFile`
// non-empty adds `program: file:`.
bool applyPreset(PC1500Machine** out, std::unique_ptr<PC1500Machine>* holder, SerialLink* link,
                 const std::string& model, const std::string& card,
                 const std::vector<std::string>& extraKeys, const std::string& loaderFile,
                 PresetLoadResult* res) {
    std::string yaml = "model: " + model + "\ninterface: CE-158\n";
    if (!card.empty()) yaml += "slot-1: " + card + "\n";
    yaml += "keys:\n  - key: cl\n  - type: NEW0\n";
    for (const std::string& k : extraKeys) yaml += "  - type: " + k + "\n";
    if (!loaderFile.empty()) yaml += "program:\n  file: " + loaderFile + "\n";
    const std::string path = std::string(kScratchDir) + "/run.pc1500";
    std::ofstream(path) << yaml;
    PresetFile preset;
    std::string err;
    if (!parsePresetFile(path, &preset, &err)) {
        res->ok = false;
        res->error = "preset: " + err;
        return false;
    }
    *holder = std::make_unique<PC1500Machine>(preset.variant);
    PC1500Machine& m = **holder;
    m.setCE158SerialLink(link);
    *res = applyPC1500Preset(m, preset, {}, ".", "Qt6/resources/cards", {}, {"roms"});
    *out = &m;
    return true;
}

const std::vector<std::string> kSerialSetup = {"SETCOM 1200", "SETDEV CI"};

Run runRom(const std::string& model, const std::string& card, bool ml, const std::vector<uint8_t>& stream) {
    Run r;
    SdeLink link;
    link.rx = stream;
    std::unique_ptr<PC1500Machine> holder;
    PC1500Machine* m = nullptr;
    PresetLoadResult res;
    std::vector<std::string> keys = kSerialSetup;
    keys.push_back(ml ? "CLOAD M" : "CLOAD");
    if (!applyPreset(&m, &holder, &link, model, card, keys, "", &res) || !res.ok) {
        r.note = "preset failed: " + res.error;
        return r;
    }
    // 300 ms at 1200 baud, 10 bits a character.
    link.pausePolls = long(0.3 * 1200 / 10 + 0.5);
    link.armed = true;
    const uint64_t chunk = PC1500Machine::kCpuHz / 10;
    uint64_t budget = PC1500Machine::kCpuHz * (60 + stream.size() / 100); // > stream time at 1200 baud
    while (!link.done() && budget > chunk) { m->runCycles(chunk); budget -= chunk; }
    m->runCycles(PC1500Machine::kCpuHz * 2);
    const std::string screen = lcd(*m);
    r.ok = link.done() && screen.find("ERROR") == std::string::npos;
    r.note = (link.done() ? "" : "stream not consumed (" + std::to_string(link.pos) + "/" +
                                 std::to_string(stream.size()) + "); ") + "lcd=[" + screen + "]";
    r.mem = snapshot(*m);
    m->setCE158SerialLink(nullptr);
    return r;
}

Run runLoader(const std::string& model, const std::string& card, const std::string& loaderFile) {
    Run r;
    std::unique_ptr<PC1500Machine> holder;
    PC1500Machine* m = nullptr;
    PresetLoadResult res;
    std::string abs = loaderFile;
    if (!abs.empty() && abs[0] != '/') {
        char buf[4096];
        if (realpath(loaderFile.c_str(), buf)) abs = buf;
    }
    if (!applyPreset(&m, &holder, nullptr, model, card, kSerialSetup, abs, &res)) {
        r.note = res.error;
        return r;
    }
    m->runCycles(PC1500Machine::kCpuHz / 2);
    r.ok = res.ok;
    r.note = (res.ok ? "" : "error: " + res.error + "; ") + "lcd=[" + lcd(*m) + "]";
    r.mem = snapshot(*m);
    return r;
}

std::string ranges(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t from, uint32_t to,
                   size_t* count) {
    std::string out;
    *count = 0;
    for (uint32_t x = from; x < to;) {
        if (a[x] == b[x]) { ++x; continue; }
        uint32_t y = x;
        while (y < to && a[y] != b[y]) ++y;
        *count += y - x;
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s&%04X-&%04X", out.empty() ? "" : ",", x, y - 1);
        out += buf;
        if (y - x <= 2) { // short range: show rom/loader values
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

int probe(const std::string& model, const std::string& card) {
    std::unique_ptr<PC1500Machine> holder;
    PC1500Machine* m = nullptr;
    PresetLoadResult res;
    if (!applyPreset(&m, &holder, nullptr, model, card, {}, "", &res) || !res.ok) {
        std::fprintf(stderr, "preset failed: %s\n", res.error.c_str());
        return 2;
    }
    const uint16_t st = peek16(*m, kBasPrgSt);
    const uint32_t ramEnd = uint32_t(m->memory().peek(kRamEnd)) << 8;
    // MEM = RAM_END*256 - (BASPRG_END + 1) - ... ; print the plain span.
    std::printf("BASPRG_ST=%04X RAM_ST=%02X00 RAM_END=%04X span=%u\n", st, m->memory().peek(kRamSt), ramEnd,
                unsigned(ramEnd - st));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string model, card, kind, streamPath, loaderFile;
    bool doProbe = false, expectRefuse = false;
    for (int i = 1; i < argc; ++i) {
        auto val = [&](std::string* dst) { if (i + 1 < argc) *dst = argv[++i]; };
        if (!std::strcmp(argv[i], "--probe")) doProbe = true;
        else if (!std::strcmp(argv[i], "--expect-refuse")) expectRefuse = true;
        else if (!std::strcmp(argv[i], "--model")) val(&model);
        else if (!std::strcmp(argv[i], "--card")) val(&card);
        else if (!std::strcmp(argv[i], "--kind")) val(&kind);
        else if (!std::strcmp(argv[i], "--stream")) val(&streamPath);
        else if (!std::strcmp(argv[i], "--loader-file")) val(&loaderFile);
        else { std::fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    if (card == "none") card.clear();
    if (model.empty()) { std::fprintf(stderr, "--model required\n"); return 2; }
    if (doProbe) return probe(model, card);

    const bool ml = kind == "ml";
    std::vector<uint8_t> stream;
    if (!readFile(streamPath, &stream) || stream.size() <= kHeaderLen) {
        std::fprintf(stderr, "cannot read stream %s\n", streamPath.c_str());
        return 2;
    }
    const std::vector<uint8_t> payload(stream.begin() + kHeaderLen, stream.end());

    Run a = runRom(model, card, ml, stream);
    Run b = runLoader(model, card, loaderFile);
    std::printf("ROM:    %s %s\n", a.ok ? "ok" : "REFUSED/FAILED", a.note.c_str());
    std::printf("LOADER: %s %s\n", b.ok ? "ok" : "REFUSED/FAILED", b.note.c_str());

    if (expectRefuse) {
        const bool pass = !a.ok && !b.ok;
        std::printf("RESULT %s (expect refuse)\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }
    if (a.mem.empty() || b.mem.empty()) {
        std::printf("RESULT FAIL (no memory snapshot)\n");
        return 1;
    }

    // The loaded range must hold the payload, in both runs.
    uint32_t loadAt;
    if (ml) {
        loadAt = uint32_t(stream[21]) << 8 | stream[22]; // CE-158 header: load address
    } else {
        loadAt = uint32_t(a.mem[kBasPrgSt]) << 8 | a.mem[kBasPrgSt + 1];
    }
    auto holds = [&](const Run& r) {
        if (loadAt + payload.size() > r.mem.size()) return false;
        return std::equal(payload.begin(), payload.end(), r.mem.begin() + loadAt);
    };
    const bool aHolds = holds(a), bHolds = holds(b);

    const uint32_t userFrom = uint32_t(a.mem[kRamSt]) << 8, userTo = uint32_t(a.mem[kRamEnd]) << 8;
    size_t nUser = 0, nPtr = 0, nSys = 0;
    const std::string dUser = ranges(a.mem, b.mem, userFrom, userTo, &nUser);
    const std::string dPtr = ranges(a.mem, b.mem, 0x7860, 0x7870, &nPtr);
    const std::string dSys = ranges(a.mem, b.mem, 0x7600, 0x8000, &nSys);

    std::printf("load at &%04X, %zu bytes; payload in RAM: rom=%s loader=%s\n", loadAt, payload.size(),
                aHolds ? "yes" : "NO", bHolds ? "yes" : "NO");
    std::printf("BASPRG_ST rom=%04X loader=%04X  BASPRG_END rom=%04X loader=%04X\n",
                a.mem[kBasPrgSt] << 8 | a.mem[kBasPrgSt + 1], b.mem[kBasPrgSt] << 8 | b.mem[kBasPrgSt + 1],
                a.mem[kBasPrgEnd] << 8 | a.mem[kBasPrgEnd + 1], b.mem[kBasPrgEnd] << 8 | b.mem[kBasPrgEnd + 1]);
    std::printf("user RAM &%04X-&%04X diff: %zu bytes %s\n", userFrom, userTo - 1, nUser, dUser.c_str());
    std::printf("pointers &7860-&786F diff: %zu bytes %s\n", nPtr, dPtr.c_str());
    std::printf("system RAM &7600-&7FFF diff (info): %zu bytes %s\n", nSys, dSys.c_str());

    const bool pass = a.ok && b.ok && aHolds && bHolds && nUser == 0 && nPtr == 0;
    std::printf("RESULT %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
