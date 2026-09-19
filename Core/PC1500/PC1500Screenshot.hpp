#pragma once
#include <cstddef>

#include "../Display/LcdScreenshot.hpp"
#include "PC1500Display.hpp"
#include "PC1500Machine.hpp"

// The PC-1500/1500A dot matrix (156 x 7) as an LcdBitmap -- see
// Core/Display/LcdScreenshot.hpp. Thread-safe: goes through the locked
// display() snapshot.
inline LcdBitmap pc1500LcdBitmap(const PC1500Machine& machine) {
    const PC1500Display disp = machine.display();
    LcdBitmap bitmap;
    bitmap.cols = PC1500Display::kCols;
    bitmap.rows = PC1500Display::kRows;
    bitmap.pixels.resize(static_cast<std::size_t>(bitmap.cols) * bitmap.rows);
    for (int row = 0; row < bitmap.rows; ++row) {
        for (int col = 0; col < bitmap.cols; ++col) {
            bitmap.pixels[static_cast<std::size_t>(row) * bitmap.cols + col] = disp.pixel(col, row);
        }
    }
    bitmap.poweredOn = machine.isDisplayOn();
    return bitmap;
}
