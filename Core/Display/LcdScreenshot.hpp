#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ── LCD screenshot: dot matrix -> physically-sized greyscale image ───────
//
// One renderer for both the GUI's Edit > Copy Screen (clipboard) and the
// `- screenshot: <file>` preset step (GUI and headless CLIs alike), so the
// two produce the same pixels. Only the dot matrix is drawn -- no status
// annunciators -- as black dots on white, sized to the real display:
// PC-1600 89 x 18 mm, PC-1500/1500A 104 x 5 mm, at 600 DPI. Each dot fills
// 0.9 of its cell (the on-screen LcdWidget's layout), antialiased by exact
// per-pixel area coverage. A powered-off display renders blank white.
//
// Model-free on purpose (pc1500_cli doesn't link the PC-1600 sources): the
// per-model extraction lives in PC1500/PC1500Screenshot.hpp and
// PC1600/PC1600Screenshot.hpp.

struct LcdBitmap {
    int cols = 0;
    int rows = 0;
    std::vector<bool> pixels; // row-major, cols*rows entries
    bool poweredOn = true;
};

struct ScreenSizeMm {
    double width;
    double height;
};

inline constexpr ScreenSizeMm kPC1500ScreenMm{104.0, 5.0};
inline constexpr ScreenSizeMm kPC1600ScreenMm{89.0, 18.0};
inline constexpr double kScreenshotDpi = 600.0;

struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // row-major, 0 = black, 255 = white
    double dpi = kScreenshotDpi;
    uint32_t pixelsPerMeter() const { return static_cast<uint32_t>(dpi / 0.0254 + 0.5); }
};

GrayImage renderLcdImage(const LcdBitmap& bitmap, ScreenSizeMm size, double dpi = kScreenshotDpi);

/// renderLcdImage() + PNG (with pHYs) to `path`, overwriting it. False
/// with `error` set if the file can't be written.
bool writeLcdScreenshotPng(const LcdBitmap& bitmap, ScreenSizeMm size, const std::string& path,
                           std::string* error);
