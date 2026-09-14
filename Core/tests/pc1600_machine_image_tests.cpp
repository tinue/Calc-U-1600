// Headless tests for Core/PC1600/PC1600MachineImage.cpp -- parsing the
// 16-byte PC-1600 machine-language transfer header (magic FF 10 00 00,
// type byte 0x10) down to its load / auto-run addresses. Same
// no-framework, assert-and-tally style as the other Core test files.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../PC1600/PC1600MachineImage.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// A tiny machine-language "payload": three RET bytes.
std::vector<uint8_t> samplePayload() { return {0xC9, 0xC9, 0xC9}; }

std::vector<uint8_t> makeHeader(const std::vector<uint8_t>& payload, uint8_t typeByte = 0x10,
                                uint32_t loadAddr = 0xD000, uint32_t autorunAddr = 0xE252,
                                uint8_t trailerLo = 0x0F) {
    std::vector<uint8_t> f(16, 0x00);
    f[0] = 0xFF; f[1] = 0x10; f[2] = 0x00; f[3] = 0x00;
    f[4] = typeByte;
    uint32_t n = static_cast<uint32_t>(payload.size());
    f[5] = static_cast<uint8_t>(n & 0xFF);
    f[6] = static_cast<uint8_t>((n >> 8) & 0xFF);
    f[7] = static_cast<uint8_t>((n >> 16) & 0xFF);
    f[8] = static_cast<uint8_t>(loadAddr & 0xFF);
    f[9] = static_cast<uint8_t>((loadAddr >> 8) & 0xFF);
    f[10] = static_cast<uint8_t>((loadAddr >> 16) & 0xFF);
    f[11] = static_cast<uint8_t>(autorunAddr & 0xFF);
    f[12] = static_cast<uint8_t>((autorunAddr >> 8) & 0xFF);
    f[13] = static_cast<uint8_t>((autorunAddr >> 16) & 0xFF);
    f[0x0E] = 0x00; f[0x0F] = trailerLo;
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

void test_valid_header_fields() {
    auto img = pc1600::parsePC1600MachineImage(makeHeader(samplePayload()));
    CHECK(img.hasHeader);
    CHECK(img.ok);
    CHECK(img.error.empty());
    CHECK(img.headerSize == 16);
    CHECK(img.headerPayloadLen == 3);
    CHECK(img.loadAddr == 0xD000);
    CHECK(img.autorunAddr == 0xE252);
}

void test_zero_autorun() {
    auto img = pc1600::parsePC1600MachineImage(
        makeHeader(samplePayload(), /*type=*/0x10, /*load=*/0x8000, /*autorun=*/0));
    CHECK(img.hasHeader && img.ok);
    CHECK(img.loadAddr == 0x8000);
    CHECK(img.autorunAddr == 0);
}

void test_length_not_checked_here() {
    // Claim one extra payload byte -- the parser must NOT reject this
    // (the loader compares against the real file size).
    auto file = makeHeader(samplePayload());
    file[5] = static_cast<uint8_t>(file[5] + 1);
    auto img = pc1600::parsePC1600MachineImage(file);
    CHECK(img.hasHeader && img.ok);
    CHECK(img.headerPayloadLen == 4);
}

void test_bad_magic_not_a_header() {
    auto file = makeHeader(samplePayload());
    file[1] = 0x11;  // magic FF 11 00 00
    auto img = pc1600::parsePC1600MachineImage(file);
    CHECK(!img.hasHeader);
    CHECK(!img.ok);
    CHECK(img.error.empty());  // silently "no header", not an error
}

void test_basic_type_byte_not_a_machine_header() {
    auto img = pc1600::parsePC1600MachineImage(makeHeader(samplePayload(), /*type=*/0x21));
    CHECK(!img.hasHeader);
    CHECK(!img.ok);
}

void test_byteswapped_trailer_accepted() {
    auto img = pc1600::parsePC1600MachineImage(
        makeHeader(samplePayload(), 0x10, 0xD000, 0xE252, /*trailerLo=*/0xF0));
    CHECK(img.hasHeader && img.ok);
    CHECK(img.loadAddr == 0xD000);
}

void test_bad_trailer_is_error() {
    auto img = pc1600::parsePC1600MachineImage(
        makeHeader(samplePayload(), 0x10, 0xD000, 0xE252, /*trailerLo=*/0x00));
    CHECK(img.hasHeader);
    CHECK(!img.ok);
    CHECK(img.error.find("end marker") != std::string::npos);
}

void test_too_short() {
    std::vector<uint8_t> few = {0xFF, 0x10, 0x00};
    auto img = pc1600::parsePC1600MachineImage(few);
    CHECK(!img.hasHeader);
    CHECK(!img.ok);
}

void test_headerless_blob() {
    auto img = pc1600::parsePC1600MachineImage(std::vector<uint8_t>(64, 0xC9));
    CHECK(!img.hasHeader);
}

}  // namespace

int run_pc1600_machine_image_tests() {
    test_valid_header_fields();
    test_zero_autorun();
    test_length_not_checked_here();
    test_bad_magic_not_a_header();
    test_basic_type_byte_not_a_machine_header();
    test_byteswapped_trailer_accepted();
    test_bad_trailer_is_error();
    test_too_short();
    test_headerless_blob();

    std::printf("pc1600_machine_image_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
