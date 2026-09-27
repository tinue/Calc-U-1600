// Headless tests for Core/Basic/BasicProgramSource.cpp -- the fast BASIC
// loaders' file reader: a plain-text `.bas` listing, tokenized headerless via
// the vendored libsharpdx with the target model as the tokenizer device, or
// tokenized BASIC behind a CE-158 / PC-1600 header.
//
// Same no-framework, assert-and-tally style as the other Core test files.
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../Basic/BasicProgramSource.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

std::string writeTemp(const std::string& name, const std::string& text) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / name;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
    return p.string();
}

// A .bas listing tokenizes to a bare run of line records: 00 0A (line 10),
// a length byte, tokens, 0x0D; no header, no trailing program-end marker.
void test_listing_tokenized() {
    std::string path = writeTemp("bps.bas", "10 PRINT \"HI\"\n20 END\n");
    basic::BasicProgramSource s = basic::readBasicProgramFile(path, basic::TransferModel::PC1500);
    CHECK(s.ok);
    CHECK(s.error.empty());
    CHECK(s.payload.size() >= 6);
    CHECK(s.payload[0] == 0x00 && s.payload[1] == 0x0A);  // line 10, big-endian
    CHECK(s.payload[2] >= 1);                             // length byte = content + 1
    CHECK(s.payload.back() == 0x0D);                      // last record ends with CR (no 0xFF marker)
}

// Both device families tokenize; PC-1600 accepts non-ASCII, PC-1500 does not.
void test_device_is_passed_through() {
    std::string path = writeTemp("bps_u.bas", "10 PRINT \"\xC3\x9C\"\n");  // UTF-8 U+00DC
    basic::BasicProgramSource p16 =
        basic::readBasicProgramFile(path, basic::TransferModel::PC1600);
    CHECK(p16.ok);

    basic::BasicProgramSource p15 =
        basic::readBasicProgramFile(path, basic::TransferModel::PC1500);
    CHECK(!p15.ok);
    CHECK(p15.error.find("7-bit") != std::string::npos);
}

// A file that isn't an ASCII BASIC listing is rejected by the tokenizer.
void test_non_listing_rejected() {
    std::string path = writeTemp("bps_prose.bas", "just some prose, not a program\n");
    basic::BasicProgramSource s = basic::readBasicProgramFile(path, basic::TransferModel::PC1500);
    CHECK(!s.ok);
    CHECK(!s.error.empty());
}

void test_missing_file_rejected() {
    basic::BasicProgramSource s = basic::readBasicProgramFile(
        "/no/such/dir/nope.bas", basic::TransferModel::PC1500);
    CHECK(!s.ok);
    CHECK(s.error.find("open") != std::string::npos);
}

// Two independently line-numbered programs concatenated with a `#SEGMENT`
// marker line -- the real-hardware mechanism behind GOSUB "LABEL" jumping
// into a second program saved right after the first (see
// SharpDataExchange's scanner.rs and docs/PC1600-Serial-Port.md).
// readBasicProgramFile() asks the tokenizer for SDE_SEGMENT_MARKER_MEMORY,
// which renders the marker as the bare 0xFF the ROM's serial receiver
// actually stores in the program area -- not the 3-byte 0xFF 0x00 0x00 wire
// form SAVE "COM1:" transmits (that trailing 0x00 0x00 is a transmission-only
// pacing marker, see sender.rs). Confirmed against the emulator's own
// LOAD "COM1:" -- BASPRG_END - BASPRG_ST == 0x52 (82), matching this
// payload's length exactly.
void test_segment_marker_tokenized() {
    std::string path = writeTemp("bps_segment.bas",
                                  "5 \"PART1\"\n"
                                  "10 \"A\"CLS : WAIT: GOSUB \"PART2\"\n"
                                  "15 \"B\"CLS :PRINT\"Part 1\"\n"
                                  "#SEGMENT\n"
                                  "5 \"PART2\":PRINT\"Part 2\"\n"
                                  "10 RETURN\n");
    basic::BasicProgramSource s = basic::readBasicProgramFile(path, basic::TransferModel::PC1600);
    CHECK(s.ok);
    if (!s.ok) {
        std::fprintf(stderr, "  error: %s\n", s.error.c_str());
        return;
    }
    const std::vector<uint8_t> want = {
        0x00, 0x05, 0x08, 0x22, 0x50, 0x41, 0x52, 0x54, 0x31, 0x22, 0x0D,
        0x00, 0x0A, 0x13, 0x22, 0x41, 0x22, 0xF0, 0x88, 0x3A, 0xF1, 0xB3, 0x3A, 0xF1, 0x94, 0x22,
        0x50, 0x41, 0x52, 0x54, 0x32, 0x22, 0x0D,
        0x00, 0x0F, 0x11, 0x22, 0x42, 0x22, 0xF0, 0x88, 0x3A, 0xF0, 0x97, 0x22, 0x50, 0x61, 0x72,
        0x74, 0x20, 0x31, 0x22, 0x0D,
        0xFF,  // #SEGMENT -> just the leading 0xFF; the pacing pad is dropped
        0x00, 0x05, 0x13, 0x22, 0x50, 0x41, 0x52, 0x54, 0x32, 0x22, 0x3A, 0xF0, 0x97, 0x22, 0x50,
        0x61, 0x72, 0x74, 0x20, 0x32, 0x22, 0x0D,
        0x00, 0x0A, 0x03, 0xF1, 0x99, 0x0D,
    };
    CHECK(s.payload == want);
    CHECK(s.payload.size() == 0x52);  // matches BASPRG_END - BASPRG_ST on real LOAD "COM1:"
}

// ── Tokenized files (.bbin) ──────────────────────────────────────────────

// 10 END / 20 PRINT "AB" as in-RAM line records.
const std::vector<uint8_t> kRecords = {
    0x00, 0x0A, 0x03, 0xF1, 0x8E, 0x0D,
    0x00, 0x14, 0x07, 0xF0, 0x97, 0x22, 0x41, 0x42, 0x22, 0x0D,
};

std::vector<uint8_t> ce158(const std::vector<uint8_t>& payload, uint8_t type = 0x40) {
    std::vector<uint8_t> f(27, 0x00);
    f[0] = 0x01;
    f[1] = type;
    f[2] = 'C'; f[3] = 'O'; f[4] = 'M';
    const uint16_t wire = static_cast<uint16_t>(payload.size() - 1);  // stored as length - 1
    f[0x17] = static_cast<uint8_t>(wire >> 8);
    f[0x18] = static_cast<uint8_t>(wire & 0xFF);
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

std::vector<uint8_t> pc1600(const std::vector<uint8_t>& payload, uint8_t type = 0x21) {
    std::vector<uint8_t> f(16, 0x00);
    f[0] = 0xFF; f[1] = 0x10;
    f[4] = type;
    f[5] = static_cast<uint8_t>(payload.size());
    f[0x0F] = 0x0F;
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

// The header decides the source machine, whatever model a listing would get.
void test_tokenized_files() {
    const basic::BasicProgramSource a = basic::readBasicProgram(ce158(kRecords), basic::TransferModel::PC1600);
    CHECK(a.ok && !a.listing);
    CHECK(a.source == basic::TransferModel::PC1500);
    CHECK(a.payload == kRecords);

    const basic::BasicProgramSource b = basic::readBasicProgram(pc1600(kRecords), basic::TransferModel::PC1500);
    CHECK(b.ok && !b.listing);
    CHECK(b.source == basic::TransferModel::PC1600);
    CHECK(b.payload == kRecords);
}

void test_tokenized_rejects() {
    // Machine code, with either header.
    const auto ml = basic::readBasicProgram(pc1600(kRecords, /*type=*/0x10), basic::TransferModel::PC1600);
    CHECK(!ml.ok);
    CHECK(ml.error.find("Load Machine Code") != std::string::npos);
    CHECK(!basic::readBasicProgram(ce158(kRecords, /*type=*/0x42), basic::TransferModel::PC1500).ok);
    // A trailing byte the length field doesn't account for.
    auto trailing = ce158(kRecords);
    trailing.push_back(0x00);
    const auto t = basic::readBasicProgram(trailing, basic::TransferModel::PC1500);
    CHECK(!t.ok);
    CHECK(t.error.find("header says") != std::string::npos);
    // Bare line records: no header, not a listing.
    const auto bare = basic::readBasicProgram(kRecords, basic::TransferModel::PC1500);
    CHECK(!bare.ok);
    CHECK(bare.error.find("neither") != std::string::npos);
    CHECK(!basic::readBasicProgram({}, basic::TransferModel::PC1500).ok);
}

// The listing and the tokenized file of the same program load the same bytes.
void test_listing_equals_tokenized() {
    std::ifstream in("Core/tests/fixtures/basic/lissajou-1600.bbin", std::ios::binary);
    std::ifstream listing("Core/tests/fixtures/basic/lissajou-1600.bas", std::ios::binary);
    if (!in || !listing) {
        std::fprintf(stderr, "  (skipped lissajou-1600 listing/tokenized pair -- not present)\n");
        return;
    }
    const std::vector<uint8_t> tok((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::vector<uint8_t> bas((std::istreambuf_iterator<char>(listing)), std::istreambuf_iterator<char>());
    const auto a = basic::readBasicProgram(tok, basic::TransferModel::PC1600);
    const auto b = basic::readBasicProgram(bas, basic::TransferModel::PC1600);
    CHECK(a.ok && b.ok);
    CHECK(a.payload == b.payload);
}

}  // namespace

int run_basic_program_source_tests() {
    test_listing_tokenized();
    test_device_is_passed_through();
    test_non_listing_rejected();
    test_missing_file_rejected();
    test_segment_marker_tokenized();
    test_tokenized_files();
    test_tokenized_rejects();
    test_listing_equals_tokenized();

    std::printf("basic_program_source_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
