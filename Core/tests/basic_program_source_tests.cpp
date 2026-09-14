// Headless tests for Core/Basic/BasicProgramSource.cpp -- the `format:
// basic-binary` program-source reader: read a plain-text `.bas` listing and
// tokenize it headerless via the vendored libsharpdx, passing the preset's
// target model through as the tokenizer device.
//
// Same no-framework, assert-and-tally style as the other Core test files.
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
    basic::BasicProgramSource s = basic::readBasicProgramSource(path, basic::TransferModel::PC1500);
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
        basic::readBasicProgramSource(path, basic::TransferModel::PC1600);
    CHECK(p16.ok);

    basic::BasicProgramSource p15 =
        basic::readBasicProgramSource(path, basic::TransferModel::PC1500);
    CHECK(!p15.ok);
    CHECK(p15.error.find("7-bit") != std::string::npos);
}

// A file that isn't an ASCII BASIC listing is rejected by the tokenizer.
void test_non_listing_rejected() {
    std::string path = writeTemp("bps_prose.bas", "just some prose, not a program\n");
    basic::BasicProgramSource s = basic::readBasicProgramSource(path, basic::TransferModel::PC1500);
    CHECK(!s.ok);
    CHECK(!s.error.empty());
}

void test_missing_file_rejected() {
    basic::BasicProgramSource s = basic::readBasicProgramSource(
        "/no/such/dir/nope.bas", basic::TransferModel::PC1500);
    CHECK(!s.ok);
    CHECK(s.error.find("open") != std::string::npos);
}

}  // namespace

int run_basic_program_source_tests() {
    test_listing_tokenized();
    test_device_is_passed_through();
    test_non_listing_rejected();
    test_missing_file_rejected();

    std::printf("basic_program_source_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
