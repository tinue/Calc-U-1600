#pragma once
#include <cstddef>

#include "../Display/LcdScreenshot.hpp"
#include "PC1600Display.hpp"
#include "PC1600Machine.hpp"

// The PC-1600 graphics area (156 x 32, status strip excluded) as an
// LcdBitmap -- see Core/Display/LcdScreenshot.hpp. Thread-safe: goes
// through the locked displaySnapshot().
inline LcdBitmap pc1600LcdBitmap(const PC1600Machine& machine) {
    const PC1600DisplaySnapshot snap = machine.displaySnapshot();
    LcdBitmap bitmap;
    bitmap.cols = PC1600Display::kWidth;
    bitmap.rows = PC1600Display::kHeight;
    bitmap.pixels.resize(static_cast<std::size_t>(bitmap.cols) * bitmap.rows);
    for (int row = 0; row < bitmap.rows; ++row) {
        for (int col = 0; col < bitmap.cols; ++col) {
            bitmap.pixels[static_cast<std::size_t>(row) * bitmap.cols + col] = snap.pixels[row][col];
        }
    }
    bitmap.poweredOn = snap.clockEnabled;
    return bitmap;
}
