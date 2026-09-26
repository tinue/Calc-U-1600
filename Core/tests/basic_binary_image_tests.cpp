// Headless tests for Core/Basic/BasicBinaryImage.cpp -- parsing a
// SharpDataExchange "tokenized BASIC" transfer file (CE-158 or PC-1600
// header) down to its payload. Same no-framework, assert-and-tally style
// as the other Core test files.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "../Basic/BasicBinaryImage.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// A 3-record tokenized payload: 10 END / 20 PRINT "AB" / 180 <tok>:END .
// Matches the in-RAM byte layout the --dump-basic oracle confirmed.
std::vector<uint8_t> samplePayload() {
    return {
        0x00, 0x0A, 0x03, 0xF1, 0x8E, 0x0D,                          // 10 END
        0x00, 0x14, 0x07, 0xF0, 0x97, 0x22, 0x41, 0x42, 0x22, 0x0D,  // 20 PRINT "AB"
        0x00, 0xB4, 0x06, 0xF1, 0xB3, 0x3A, 0xF1, 0x8E, 0x0D,        // 180 <tok>:END
    };
}

std::vector<uint8_t> makeCe158(const std::vector<uint8_t>& payload, uint8_t typeByte = 0x40) {
    std::vector<uint8_t> f(27, 0x00);
    f[0] = 0x01;
    f[1] = typeByte;
    f[2] = 'C'; f[3] = 'O'; f[4] = 'M';
    const char* name = "SAMPLE";
    for (int i = 0; name[i] && i < 16; i++) f[5 + i] = static_cast<uint8_t>(name[i]);
    // length field 0x17..0x18, big-endian, capacity-1
    uint16_t wire = static_cast<uint16_t>(payload.size() - 1);
    f[0x17] = static_cast<uint8_t>(wire >> 8);
    f[0x18] = static_cast<uint8_t>(wire & 0xFF);
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

std::vector<uint8_t> makePc1600(const std::vector<uint8_t>& payload, uint8_t typeByte = 0x21) {
    std::vector<uint8_t> f(16, 0x00);
    f[0] = 0xFF; f[1] = 0x10; f[2] = 0x00; f[3] = 0x00;
    f[4] = typeByte;
    uint32_t n = static_cast<uint32_t>(payload.size());
    f[5] = static_cast<uint8_t>(n & 0xFF);
    f[6] = static_cast<uint8_t>((n >> 8) & 0xFF);
    f[7] = static_cast<uint8_t>((n >> 16) & 0xFF);
    f[0x0E] = 0x00; f[0x0F] = 0x0F;
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

void test_ce158_roundtrip() {
    auto payload = samplePayload();
    auto file = makeCe158(payload);
    auto img = basic::parseBasicBinaryTransfer(file);
    CHECK(img.ok);
    CHECK(img.error.empty());
    CHECK(img.model == basic::TransferModel::PC1500);
    CHECK(img.payload == payload);
}

void test_pc1600_roundtrip() {
    auto payload = samplePayload();
    auto file = makePc1600(payload);
    auto img = basic::parseBasicBinaryTransfer(file);
    CHECK(img.ok);
    CHECK(img.model == basic::TransferModel::PC1600);
    CHECK(img.payload == payload);
}

void test_single_record_program() {
    std::vector<uint8_t> oneLine = {0x00, 0x0A, 0x03, 0xF1, 0x8E, 0x0D};  // 10 END
    auto img = basic::parseBasicBinaryTransfer(makeCe158(oneLine));
    CHECK(img.ok);
    CHECK(img.payload == oneLine);
}

void test_rejects_wrong_ce158_type_byte() {
    auto file = makeCe158(samplePayload(), /*typeByte=*/0x42);  // MACHINE
    auto img = basic::parseBasicBinaryTransfer(file);
    CHECK(!img.ok);
    CHECK(img.error.find("0x42") != std::string::npos);
}

void test_rejects_wrong_pc1600_type_byte() {
    auto file = makePc1600(samplePayload(), /*typeByte=*/0x10);  // MACHINE
    auto img = basic::parseBasicBinaryTransfer(file);
    CHECK(!img.ok);
    CHECK(img.error.find("0x10") != std::string::npos);
}

void test_rejects_length_mismatch_ce158() {
    auto file = makeCe158(samplePayload());
    file.push_back(0x00);  // one trailing byte the length field doesn't account for
    auto img = basic::parseBasicBinaryTransfer(file);
    CHECK(!img.ok);
    CHECK(img.error.find("length field") != std::string::npos);
}

void test_rejects_length_mismatch_pc1600() {
    auto file = makePc1600(samplePayload());
    file[5] = static_cast<uint8_t>(file[5] + 1);  // claim one more payload byte than present
    auto img = basic::parseBasicBinaryTransfer(file);
    CHECK(!img.ok);
}

void test_rejects_pc1600_bad_trailer() {
    auto file = makePc1600(samplePayload());
    file[0x0F] = 0x00;
    auto img = basic::parseBasicBinaryTransfer(file);
    CHECK(!img.ok);
    CHECK(img.error.find("trailer") != std::string::npos);
}

// The SharpDataExchange `convert` toolchain writes the trailer byte-swapped
// (00 F0). Accepted, same as the reference-fixture 00 0F.
void test_accepts_pc1600_byteswapped_trailer() {
    auto file = makePc1600(samplePayload());
    file[0x0E] = 0x00;
    file[0x0F] = 0xF0;
    auto img = basic::parseBasicBinaryTransfer(file);
    CHECK(img.ok);
    CHECK(img.payload == samplePayload());
}

void test_rejects_headerless_payload() {
    auto img = basic::parseBasicBinaryTransfer(samplePayload());
    CHECK(!img.ok);
    CHECK(img.error.find("unrecognized") != std::string::npos);
}

void test_rejects_empty_input() {
    std::vector<uint8_t> nothing;
    auto img = basic::parseBasicBinaryTransfer(nothing);
    CHECK(!img.ok);
}

// Real SharpDataExchange output, if the sample files are checked in.
// Skipped (not failed) when they are absent, like the ROM-gated tests.
bool readBytes(const char* path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    *out = std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return !out->empty();
}

void test_real_sample_files() {
    struct Case { const char* path; basic::TransferModel model; };
    const Case cases[] = {
        {"Core/tests/fixtures/basic/lissajou-1500_tokenized.bas", basic::TransferModel::PC1500},
        {"Core/tests/fixtures/basic/lissajou-1600_tokenized.bas", basic::TransferModel::PC1600},
    };
    for (const auto& c : cases) {
        std::vector<uint8_t> bytes;
        if (!readBytes(c.path, &bytes)) {
            std::fprintf(stderr, "  (skipped %s -- not present)\n", c.path);
            continue;
        }
        auto img = basic::parseBasicBinaryTransfer(bytes);
        CHECK(img.ok);
        CHECK(img.model == c.model);
        // First record: line 10, a REM (F1 AB).
        CHECK(img.payload.size() > 5);
        CHECK(img.payload[0] == 0x00 && img.payload[1] == 0x0A);
        CHECK(img.payload[3] == 0xF1 && img.payload[4] == 0xAB);
        // Last payload byte is a line terminator, not an end marker.
        CHECK(img.payload.back() == 0x0D);
    }
}

}  // namespace

int run_basic_binary_image_tests() {
    test_ce158_roundtrip();
    test_pc1600_roundtrip();
    test_single_record_program();
    test_rejects_wrong_ce158_type_byte();
    test_rejects_wrong_pc1600_type_byte();
    test_rejects_length_mismatch_ce158();
    test_rejects_length_mismatch_pc1600();
    test_rejects_pc1600_bad_trailer();
    test_accepts_pc1600_byteswapped_trailer();
    test_rejects_headerless_payload();
    test_rejects_empty_input();
    test_real_sample_files();

    std::printf("basic_binary_image_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
