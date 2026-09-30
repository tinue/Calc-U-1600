// Headless tests for Core/ProgramFile -- what libsharpdx's sde_file_info
// says a program file is, as the loaders see it. Same no-framework,
// assert-and-tally style as the other Core test files.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../DropFile.hpp"
#include "../ProgramFile.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using programfile::Kind;
using programfile::classify;

// A 3-record tokenized payload: 10 END / 20 PRINT "AB" / 180 <tok>:END .
std::vector<uint8_t> basicPayload() {
    return {
        0x00, 0x0A, 0x03, 0xF1, 0x8E, 0x0D,                          // 10 END
        0x00, 0x14, 0x07, 0xF0, 0x97, 0x22, 0x41, 0x42, 0x22, 0x0D,  // 20 PRINT "AB"
        0x00, 0xB4, 0x06, 0xF1, 0xB3, 0x3A, 0xF1, 0x8E, 0x0D,        // 180 <tok>:END
    };
}

const std::vector<uint8_t> kCode = {0x3E, 0x41, 0xC9, 0x00, 0x11};  // 5 bytes

// CE-158 header: type '@' BASIC, 'B' machine code; length stored as length - 1.
std::vector<uint8_t> ce158(const std::vector<uint8_t>& payload, uint8_t type, uint16_t load = 0,
                           uint16_t autorun = 0) {
    std::vector<uint8_t> f(27, 0x00);
    f[0] = 0x01;
    f[1] = type;
    f[2] = 'C'; f[3] = 'O'; f[4] = 'M';
    const char* name = "SAMPLE";
    for (int i = 0; name[i]; i++) f[5 + i] = static_cast<uint8_t>(name[i]);
    const uint16_t wire = static_cast<uint16_t>(payload.size() - 1);
    for (auto [at, v] : {std::pair<int, uint16_t>{0x15, load}, {0x17, wire}, {0x19, autorun}}) {
        f[at] = static_cast<uint8_t>(v >> 8);
        f[at + 1] = static_cast<uint8_t>(v & 0xFF);
    }
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

// PC-1600 header: type 0x21 BASIC, 0x10 machine code; 24-bit little-endian fields.
std::vector<uint8_t> pc1600(const std::vector<uint8_t>& payload, uint8_t type, uint32_t load = 0,
                            uint32_t autorun = 0) {
    std::vector<uint8_t> f = {0xFF, 0x10, 0x00, 0x00, type};
    for (uint32_t v : {static_cast<uint32_t>(payload.size()), load, autorun}) {
        f.push_back(static_cast<uint8_t>(v & 0xFF));
        f.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        f.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    }
    f.push_back(0x00);
    f.push_back(0x0F);
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

std::vector<uint8_t> text(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

void test_kinds() {
    CHECK(classify({}).kind == Kind::Empty);
    CHECK(classify(text("10 PRINT \"HI\"\n20 END\n")).kind == Kind::BasicListing);
    CHECK(classify(ce158(basicPayload(), 0x40)).kind == Kind::BasicPC1500);
    CHECK(classify(pc1600(basicPayload(), 0x21)).kind == Kind::BasicPC1600);
    CHECK(classify(ce158(kCode, 0x42, 0x40C5)).kind == Kind::CodeLH5801);
    CHECK(classify(pc1600(kCode, 0x10, 0xC0C5)).kind == Kind::CodeZ80);
    CHECK(classify(kCode).kind == Kind::Headerless);
    const programfile::ProgramFile prose = classify(text("Just some notes,\nnot a program.\n"));
    CHECK(prose.kind == Kind::Other);
    CHECK(prose.token == "text");
}

void test_headered_payload_and_fields() {
    const auto ce = classify(ce158(kCode, 0x42, 0x40C5, 0x40C7));
    CHECK(!ce.damaged && !ce.lengthMismatch);
    CHECK(ce.payload == kCode);
    CHECK(ce.loadAddr == 0x40C5);
    CHECK(ce.autorunAddr == 0x40C7);

    const auto pc = classify(pc1600(kCode, 0x10, 0x02'8100, 0x02'8105));
    CHECK(pc.payload == kCode);
    CHECK(pc.loadAddr == 0x02'8100);  // bank in bits 16-23
    CHECK(pc.autorunAddr == 0x02'8105);

    const auto bas = classify(pc1600(basicPayload(), 0x21));
    CHECK(bas.payload == basicPayload());
    CHECK(bas.loadAddr == 0 && bas.autorunAddr == 0);
}

// 0 and FFFF (bank-qualified FFFFFF on the PC-1600) both mean "no auto-start".
void test_autorun_none() {
    CHECK(classify(ce158(kCode, 0x42, 0x40C5, 0)).autorunAddr == 0);
    CHECK(classify(ce158(kCode, 0x42, 0x40C5, 0xFFFF)).autorunAddr == 0);
    CHECK(classify(pc1600(kCode, 0x10, 0xC0C5, 0)).autorunAddr == 0);
    CHECK(classify(pc1600(kCode, 0x10, 0xC0C5, 0xFFFF)).autorunAddr == 0);
    CHECK(classify(pc1600(kCode, 0x10, 0x01'C0C5, 0x01'FFFF)).autorunAddr == 0);
    CHECK(classify(pc1600(kCode, 0x10, 0xC0C5, 0xFF'FFFF)).autorunAddr == 0);
}

// A length mismatch keeps everything after the header, for a preset's `length:`.
void test_length_mismatch() {
    auto trailing = ce158(kCode, 0x42, 0x40C5);
    trailing.push_back(0xAA);
    const auto t = classify(trailing);
    CHECK(t.kind == Kind::CodeLH5801);
    CHECK(t.lengthMismatch && !t.truncated && !t.damaged);
    CHECK(t.payload.size() == kCode.size() + 1);
    CHECK(t.headerPayloadLen == kCode.size());

    auto truncated = pc1600(kCode, 0x10, 0xC0C5);
    truncated.pop_back();
    const auto c = classify(truncated);
    CHECK(c.kind == Kind::CodeZ80);
    CHECK(c.lengthMismatch && c.truncated && !c.damaged);
    CHECK(c.payload.size() == kCode.size() - 1);
}

// `00` bytes before the header are skipped.
void test_leading_noise() {
    auto noisy = pc1600(kCode, 0x10, 0xC0C5);
    noisy.insert(noisy.begin(), {0x00, 0x00, 0x00});
    const auto f = classify(noisy);
    CHECK(f.kind == Kind::CodeZ80);
    CHECK(!f.damaged && !f.lengthMismatch);
    CHECK(f.payload == kCode);
}

// Header magic without a complete header of a known type.
void test_header_cut() {
    auto unknownType = pc1600(kCode, 0x55);
    CHECK(classify(unknownType).damaged);
    auto shortCe158 = ce158(kCode, 0x42);
    shortCe158.resize(20);
    CHECK(classify(shortCe158).damaged);
}

// The PC-1600 end marker (00 0F, or the 00 F0 older tools wrote) isn't checked.
void test_pc1600_end_marker_ignored() {
    auto f = pc1600(kCode, 0x10, 0xC0C5);
    f[0x0F] = 0xF0;
    CHECK(classify(f).kind == Kind::CodeZ80);
    f[0x0F] = 0x00;
    CHECK(classify(f).kind == Kind::CodeZ80);
}

// SharpDataExchange's own Z80 sample (src/cpu_guess.rs Z80_BLOCK): a loop
// with JR/DJNZ targets inside it, a CALL and a RET.
std::vector<uint8_t> z80Code() {
    static const uint8_t kBlock[] = {0x21, 0x00, 0x80, 0x06, 0x10, 0x7E, 0xFE, 0x20, 0x28,
                                     0x01, 0x23, 0x10, 0xF8, 0xCD, 0x00, 0x10, 0xC9};
    std::vector<uint8_t> out;
    for (int i = 0; i < 8; i++) out.insert(out.end(), std::begin(kBlock), std::end(kBlock));
    return out;
}

std::vector<uint8_t> pseudoRandom(size_t len, uint32_t seed) {
    std::vector<uint8_t> out(len);
    for (uint8_t& b : out) {
        seed = seed * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(seed >> 24);
    }
    return out;
}

// The CPU guess survives only as "looks like code", and never as a Kind.
void test_looks_like_code() {
    const auto code = classify(z80Code());
    CHECK(code.kind == Kind::Headerless);
    CHECK(code.looksLikeCode);
    CHECK(!classify(kCode).looksLikeCode);  // under 16 bytes: never guessed
    CHECK(!classify(pseudoRandom(4096, 7)).looksLikeCode);
    CHECK(!classify(pc1600(z80Code(), 0x10, 0xC0C5)).looksLikeCode);  // headered: no guess
}

std::vector<uint8_t> readFixture(const char* path, bool* ok) {
    std::ifstream in(path, std::ios::binary);
    *ok = static_cast<bool>(in);
    if (!in) std::fprintf(stderr, "  (skipped %s -- not present)\n", path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Core/DropFile: which loader a dropped file goes to.
void test_drop_targets() {
    using dropfile::Target;
    const auto drop = [](const std::vector<uint8_t>& bytes) { return dropfile::classify(bytes); };

    // Presets, by their top-level `model:` key.
    CHECK(drop(text("# Hanoi\nmodel: PC-1600\nkeys:\n  - key: mode\n")) == Target::Preset);
    CHECK(drop(text("model: PC-1500:A04\n")) == Target::Preset);
    CHECK(drop(text("\xEF\xBB\xBFmodel: PC-1500\n")) == Target::Preset);             // BOM
    CHECK(drop(text("# \xE2\x80\x93" "empty\xE2\x80\x93\r\nmodel: PC-1600\r\n")) == Target::Preset);  // '–', CRLF
    CHECK(drop(text("keys:\n  model: PC-1600\n")) == Target::None);                   // indented
    CHECK(drop(text("# model: PC-1600\nkeys:\n")) == Target::None);                    // commented
    CHECK(drop(text("plotter: ce150\n")) == Target::None);                             // no model
    std::vector<uint8_t> withNul = text("model: PC-1600\n");
    withNul.push_back(0x00);
    CHECK(drop(withNul) != Target::Preset);

    // BASIC: listings and tokenized files.
    CHECK(drop(text("10 PRINT \"HI\"\n20 END\n")) == Target::BasicProgram);
    CHECK(drop(ce158(basicPayload(), 0x40)) == Target::BasicProgram);
    CHECK(drop(pc1600(basicPayload(), 0x21)) == Target::BasicProgram);
    for (const char* path : {"Core/tests/fixtures/basic/lissajou-1500.bas", "Core/tests/fixtures/basic/lissajou-1600.bbin"}) {
        bool ok = false;
        const auto bytes = readFixture(path, &ok);
        if (ok) CHECK(drop(bytes) == Target::BasicProgram);
    }

    // Machine code: headered, or headerless that looks like code.
    CHECK(drop(ce158(kCode, 0x42, 0x40C5)) == Target::MachineCode);
    CHECK(drop(pc1600(kCode, 0x10, 0xC0C5)) == Target::MachineCode);
    CHECK(drop(z80Code()) == Target::MachineCode);

    // Everything else is ignored.
    CHECK(drop({}) == Target::None);
    CHECK(drop(kCode) == Target::None);                  // too short to look like code
    CHECK(drop(pseudoRandom(4096, 7)) == Target::None);  // data
    CHECK(drop(text("Just some notes,\nnot a program.\n")) == Target::None);
}

// Real SharpDataExchange output. Skipped (not failed) when absent.
void test_real_sample_files() {
    struct Case { const char* path; Kind kind; };
    const Case cases[] = {
        {"Core/tests/fixtures/basic/lissajou-1500.bbin", Kind::BasicPC1500},
        {"Core/tests/fixtures/basic/lissajou-1600.bbin", Kind::BasicPC1600},
    };
    for (const auto& c : cases) {
        std::ifstream in(c.path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "  (skipped %s -- not present)\n", c.path);
            continue;
        }
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto f = classify(bytes);
        CHECK(f.kind == c.kind);
        CHECK(!f.damaged && !f.lengthMismatch);
        // First record: line 10, a REM (F1 AB); the last byte ends a line.
        CHECK(f.payload.size() > 5);
        CHECK(f.payload[0] == 0x00 && f.payload[1] == 0x0A);
        CHECK(f.payload[3] == 0xF1 && f.payload[4] == 0xAB);
        CHECK(f.payload.back() == 0x0D);
    }
}

}  // namespace

int run_program_file_tests() {
    test_kinds();
    test_headered_payload_and_fields();
    test_autorun_none();
    test_length_mismatch();
    test_leading_noise();
    test_header_cut();
    test_pc1600_end_marker_ignored();
    test_real_sample_files();
    test_looks_like_code();
    test_drop_targets();

    std::printf("program_file_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
