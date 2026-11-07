#pragma once
#include <cstdint>

namespace shared {

// An opaque colour a renderer paints with (QColor / CSS rgb()).
struct Rgb {
    std::uint8_t r, g, b;
};

} // namespace shared
