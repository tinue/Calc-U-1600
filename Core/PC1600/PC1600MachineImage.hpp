#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ── PC-1600 machine-language transfer-file header ──────────────────────
//
// The PC-1600 wraps a saved machine-language block (CSAVE M / a captured
// serial transfer) in the same 16-byte header shape as its tokenized-BASIC
// transfers, distinguished by the type byte at 0x04
// (SharpPC1500Reference/Data-Formats/Binary-Exchange-Formats.md §3):
//
//   0x00..0x03  magic  FF 10 00 00
//   0x04        type    0x10 = MACHINE   (0x21 = tokenized BASIC)
//   0x05..0x07  payload length, little-endian, direct byte count
//   0x08..0x0A  load start address, little-endian     (Z-80 / SC7852 view)
//   0x0B..0x0D  auto-run address, little-endian, 0 = none   (ditto)
//   0x0E..0x0F  end marker  00 0F   (SharpDataExchange `convert` emits 00 F0)
//
// Addresses are Z-80-native -- BASIC's `CALL [#<bank>,]<address>` and
// `CSAVE M ...;#<bank>,<start>,<end>[,<auto-start>]` both work in the
// "Z-80A / PC-1600 address space" (`XCALL` is the LH5803 one). So callers
// apply NO +$8000 conversion, unlike the BASIC program-area pointers.
//
// `Core/Basic/BasicBinaryImage.cpp` already recognises this framing but
// only accepts the BASIC type byte and never surfaces the address fields;
// this is the MACHINE-side counterpart. Length consistency is deliberately
// NOT checked here -- machinecode::readFile() (Core/MachineCodeFile.hpp)
// compares against the actual file size, and a preset's explicit
// `length:` can override a mismatch.

namespace pc1600 {

struct MachineImage {
    // True once the magic (FF 10 00 00) AND the MACHINE type byte (0x10)
    // both match. A tokenized-BASIC file (type 0x21), a headerless blob, or
    // anything else leaves this false -- not an error; the caller then
    // needs a preset-supplied load address + length.
    bool        hasHeader = false;
    // Meaningful only when hasHeader: false => the header is present but
    // malformed (bad end marker) and `error` says how.
    bool        ok = false;
    uint32_t    loadAddr = 0;          // 0x08..0x0A
    uint32_t    autorunAddr = 0;       // 0x0B..0x0D, 0 = none
    uint32_t    headerPayloadLen = 0;  // 0x05..0x07
    std::size_t headerSize = 0;        // 16 when hasHeader, else 0
    std::string error;
};

MachineImage parsePC1600MachineImage(const uint8_t* data, std::size_t len);

inline MachineImage parsePC1600MachineImage(const std::vector<uint8_t>& bytes) {
    return parsePC1600MachineImage(bytes.data(), bytes.size());
}

}  // namespace pc1600
