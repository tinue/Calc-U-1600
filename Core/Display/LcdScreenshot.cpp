#include "LcdScreenshot.hpp"

#include <algorithm>
#include <cmath>

#include "PngWriter.hpp"

namespace {
constexpr double kDotFill = 0.9; // matches LcdWidget::paintDotMatrix
} // namespace

GrayImage renderLcdImage(const LcdBitmap& bitmap, ScreenSizeMm size, double dpi) {
    GrayImage image;
    image.dpi = dpi;
    image.width = std::max(1, static_cast<int>(std::lround(size.width / 25.4 * dpi)));
    image.height = std::max(1, static_cast<int>(std::lround(size.height / 25.4 * dpi)));
    const std::size_t count = static_cast<std::size_t>(image.width) * image.height;
    if (!bitmap.poweredOn || bitmap.cols <= 0 || bitmap.rows <= 0 ||
        bitmap.pixels.size() < static_cast<std::size_t>(bitmap.cols) * bitmap.rows) {
        image.pixels.assign(count, 255);
        return image;
    }

    // Ink coverage per output pixel; dots never overlap, so plain sums.
    std::vector<float> ink(count, 0.0f);
    const double cellW = static_cast<double>(image.width) / bitmap.cols;
    const double cellH = static_cast<double>(image.height) / bitmap.rows;
    const double dot = std::min(cellW, cellH) * kDotFill;

    for (int row = 0; row < bitmap.rows; ++row) {
        for (int col = 0; col < bitmap.cols; ++col) {
            if (!bitmap.pixels[static_cast<std::size_t>(row) * bitmap.cols + col]) continue;
            const double x0 = (col + 0.5) * cellW - dot / 2, x1 = x0 + dot;
            const double y0 = (row + 0.5) * cellH - dot / 2, y1 = y0 + dot;
            const int px0 = std::max(0, static_cast<int>(std::floor(x0)));
            const int px1 = std::min(image.width - 1, static_cast<int>(std::ceil(x1)) - 1);
            const int py0 = std::max(0, static_cast<int>(std::floor(y0)));
            const int py1 = std::min(image.height - 1, static_cast<int>(std::ceil(y1)) - 1);
            for (int py = py0; py <= py1; ++py) {
                const double cy = std::min<double>(py + 1, y1) - std::max<double>(py, y0);
                if (cy <= 0) continue;
                for (int px = px0; px <= px1; ++px) {
                    const double cx = std::min<double>(px + 1, x1) - std::max<double>(px, x0);
                    if (cx <= 0) continue;
                    ink[static_cast<std::size_t>(py) * image.width + px] += static_cast<float>(cx * cy);
                }
            }
        }
    }

    image.pixels.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float c = std::min(1.0f, ink[i]);
        image.pixels[i] = static_cast<uint8_t>(std::lround(255.0f * (1.0f - c)));
    }
    return image;
}

bool writeLcdScreenshotPng(const LcdBitmap& bitmap, ScreenSizeMm size, const std::string& path,
                           std::string* error) {
    const GrayImage image = renderLcdImage(bitmap, size);
    return writePngGray8(path, image.pixels, image.width, image.height, image.pixelsPerMeter(), error);
}
