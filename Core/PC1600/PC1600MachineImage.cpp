#include "PC1600MachineImage.hpp"

#include <cstdio>

#include "../HexFormat.hpp"

namespace pc1600 {

namespace {

uint32_t le24(const uint8_t* d) {
    return static_cast<uint32_t>(d[0]) | static_cast<uint32_t>(d[1]) << 8 |
           static_cast<uint32_t>(d[2]) << 16;
}

}  // namespace

MachineImage parsePC1600MachineImage(const uint8_t* data, std::size_t len) {
    MachineImage img;
    if (data == nullptr || len < 16) return img;  // too short to carry a header

    // Magic FF 10 00 00 + MACHINE type byte 0x10 at 0x04.
    if (!(data[0] == 0xFF && data[1] == 0x10 && data[2] == 0x00 && data[3] == 0x00 &&
          data[4] == 0x10)) {
        return img;  // not a PC-1600 MACHINE header -- hasHeader stays false
    }

    img.hasHeader = true;
    img.headerSize = 16;

    // End marker 0x0E..0x0F: `00 0F`, or the byte-swapped `00 F0` the
    // SharpDataExchange `convert` toolchain emits (same tolerance as
    // Core/Basic/BasicBinaryImage.cpp).
    const bool trailerOk = (data[0x0E] == 0x00 && data[0x0F] == 0x0F) ||
                           (data[0x0E] == 0x00 && data[0x0F] == 0xF0);
    if (!trailerOk) {
        img.error = "malformed PC-1600 MACHINE header (end marker " + hex2(data[0x0E]) + " " +
                    hex2(data[0x0F]) + ", expected 00 0F or 00 F0)";
        return img;
    }

    img.headerPayloadLen = le24(data + 0x05);
    img.loadAddr = le24(data + 0x08);
    img.autorunAddr = le24(data + 0x0B);
    img.ok = true;
    return img;
}

}  // namespace pc1600
