// Headless C++ tests for Core/Display/LcdScreenshot + PngWriter and the
// `- screenshot:` preset step. The PNG's IDAT is decoded with the system
// zlib (test-only dependency) to prove the hand-rolled deflate round-trips
// the exact pixels. ROM-gated tests SKIP (not fail) when images are absent.
//
// Build & run: see tools/run_tests.sh

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../Display/LcdScreenshot.hpp"
#include "../Display/PngWriter.hpp"
#include "../PC1500/PC1500Machine.hpp"
#include "../PC1500/PC1500PresetLoader.hpp"
#include "../PC1500/PC1500Screenshot.hpp"
#include "PresetTestSupport.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

uint32_t be32(const std::vector<uint8_t>& b, std::size_t at) {
    return (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) | (uint32_t(b[at + 2]) << 8) | b[at + 3];
}

struct DecodedPng {
    bool ok = false;
    uint32_t width = 0, height = 0, ppmX = 0, ppmY = 0;
    std::vector<uint8_t> pixels;
};

// Walks the chunks (checking every CRC), then inflates IDAT with zlib and
// strips the per-row filter bytes (all must be 0 = None).
DecodedPng decodePng(const std::vector<uint8_t>& png) {
    DecodedPng out;
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (png.size() < 8 || !std::equal(kSig, kSig + 8, png.begin())) return out;
    std::vector<uint8_t> idat;
    std::size_t at = 8;
    bool sawEnd = false;
    while (at + 12 <= png.size()) {
        const uint32_t len = be32(png, at);
        if (at + 12 + len > png.size()) return out;
        const std::string type(png.begin() + at + 4, png.begin() + at + 8);
        if (png_detail::crc32(png.data() + at + 4, len + 4) != be32(png, at + 8 + len)) return out;
        const std::size_t data = at + 8;
        if (type == "IHDR") {
            out.width = be32(png, data);
            out.height = be32(png, data + 4);
            if (png[data + 8] != 8 || png[data + 9] != 0) return out;
        } else if (type == "pHYs") {
            out.ppmX = be32(png, data);
            out.ppmY = be32(png, data + 4);
        } else if (type == "IDAT") {
            idat.insert(idat.end(), png.begin() + data, png.begin() + data + len);
        } else if (type == "IEND") {
            sawEnd = true;
        }
        at += 12 + len;
    }
    if (!sawEnd) return out;
    std::vector<uint8_t> raw(static_cast<std::size_t>(out.width + 1) * out.height);
    uLongf rawLen = raw.size();
    if (uncompress(raw.data(), &rawLen, idat.data(), idat.size()) != Z_OK || rawLen != raw.size()) return out;
    for (uint32_t y = 0; y < out.height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * (out.width + 1);
        if (raw[row] != 0) return out;
        out.pixels.insert(out.pixels.end(), raw.begin() + row + 1, raw.begin() + row + 1 + out.width);
    }
    out.ok = true;
    return out;
}

LcdBitmap checkerboard(int cols, int rows) {
    LcdBitmap b;
    b.cols = cols;
    b.rows = rows;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) b.pixels.push_back(((r + c) & 1) == 0);
    return b;
}

void test_render_size_per_model() {
    const GrayImage p16 = renderLcdImage(checkerboard(156, 32), kPC1600ScreenMm);
    CHECK(p16.width == 2102 && p16.height == 425); // 89 x 18 mm at 600 DPI
    const GrayImage p15 = renderLcdImage(checkerboard(156, 7), kPC1500ScreenMm);
    CHECK(p15.width == 2457 && p15.height == 118); // 104 x 5 mm at 600 DPI
    CHECK(p16.pixelsPerMeter() == 23622);
}

void test_render_off_is_white_and_dot_layout() {
    LcdBitmap off = checkerboard(156, 32);
    off.poweredOn = false;
    const GrayImage blank = renderLcdImage(off, kPC1600ScreenMm);
    bool allWhite = true;
    for (uint8_t v : blank.pixels) allWhite = allWhite && v == 255;
    CHECK(allWhite);

    // One lit dot at (col 10, row 5): black at its cell centre, white
    // between it and the unlit neighbour cell, white in that neighbour.
    LcdBitmap one;
    one.cols = 156;
    one.rows = 32;
    one.pixels.assign(156 * 32, false);
    one.pixels[5 * 156 + 10] = true;
    const GrayImage img = renderLcdImage(one, kPC1600ScreenMm);
    const double cw = double(img.width) / 156, ch = double(img.height) / 32;
    auto at = [&](double x, double y) { return img.pixels[std::size_t(int(y)) * img.width + std::size_t(int(x))]; };
    CHECK(at(10.5 * cw, 5.5 * ch) == 0);
    CHECK(at(11.0 * cw, 5.5 * ch) == 255); // cell boundary lies in the 10% gap
    CHECK(at(11.5 * cw, 5.5 * ch) == 255);
}

void test_png_roundtrip() {
    const GrayImage img = renderLcdImage(checkerboard(156, 32), kPC1600ScreenMm);
    const std::vector<uint8_t> png = encodePngGray8(img.pixels, img.width, img.height, img.pixelsPerMeter());
    const DecodedPng d = decodePng(png);
    CHECK(d.ok);
    CHECK(d.width == 2102 && d.height == 425);
    CHECK(d.ppmX == 23622 && d.ppmY == 23622);
    CHECK(d.pixels == img.pixels);
    CHECK(png.size() < 200 * 1024); // run matches keep it far below the ~0.9 MB raw size

    // Odd sizes and noise-like data (long literal stretches, short runs).
    std::vector<uint8_t> noise(37 * 5);
    uint32_t x = 12345;
    for (auto& v : noise) { x = x * 1103515245u + 12345u; v = static_cast<uint8_t>(x >> 16); }
    const DecodedPng dn = decodePng(encodePngGray8(noise, 37, 5, 0));
    CHECK(dn.ok && dn.pixels == noise && dn.ppmX == 0);
}

void test_preset_screenshot_parse() {
    PresetFile p;
    std::string err;
    CHECK(parsePresetString("model: PC-1500A\nkeys:\n  - screenshot: shot.png\n",
                            "/tmp/lcd_screenshot_tests_scratch.pc1500a", &p, &err));
    CHECK(!p.sections.empty() && !p.sections[0].keys.empty() &&
          p.sections[0].keys[0].kind == PresetStep::Kind::Screenshot && p.sections[0].keys[0].text == "shot.png");

    PresetFile bad;
    CHECK(!parsePresetString("model: PC-1500A\nkeys:\n  - screenshot: sub/shot.png\n",
                             "/tmp/lcd_screenshot_tests_scratch.pc1500a", &bad, &err));
    CHECK(err.find("path separator") != std::string::npos);
}

// ROM-gated: the step writes a decodable, correctly sized PNG into traceDir
// that matches a direct render of the same display.
void test_preset_screenshot_writes_png() {
    PC1500Machine m;
    PresetFile p;
    std::string err;
    CHECK(parsePresetString("model: PC-1500A\nkeys:\n  - type: PRINT 12345\n  - screenshot: lcd_shot.png\n",
                            "/tmp/lcd_screenshot_tests_scratch.pc1500a", &p, &err));
    std::remove("/tmp/lcd_shot.png");
    const PresetLoadResult r = applyPC1500Preset(m, p, {}, "/tmp", ".", {}, {"roms"});
    if (!r.ok && r.error.find("ROM") != std::string::npos) {
        std::fprintf(stderr, "SKIP test_preset_screenshot_writes_png: %s\n", r.error.c_str());
        return;
    }
    CHECK(r.ok);
    std::ifstream in("/tmp/lcd_shot.png", std::ios::binary);
    const std::vector<uint8_t> png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const DecodedPng d = decodePng(png);
    CHECK(d.ok && d.width == 2457 && d.height == 118);
    const GrayImage direct = renderLcdImage(pc1500LcdBitmap(m), kPC1500ScreenMm);
    CHECK(d.pixels == direct.pixels);
    bool anyInk = false;
    for (uint8_t v : d.pixels) anyInk = anyInk || v < 128;
    CHECK(anyInk); // "12345" is on screen
}

} // namespace

int run_lcd_screenshot_tests() {
    test_render_size_per_model();
    test_render_off_is_white_and_dot_layout();
    test_png_roundtrip();
    test_preset_screenshot_parse();
    test_preset_screenshot_writes_png();

    std::printf("lcd_screenshot_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
