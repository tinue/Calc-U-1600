#include "BasicBinaryImage.hpp"

#include <cstdio>

#include "../HexFormat.hpp"

namespace basic {

namespace {

BasicBinaryImage fail(const std::string& msg) {
    BasicBinaryImage img;
    img.ok = false;
    img.error = msg;
    return img;
}

// CE-158 header: 0x00=0x01, 0x02..0x04="COM"; type byte at 0x01.
bool looksLikeCe158(const uint8_t* d, size_t len) {
    return len >= 27 && d[0] == 0x01 && d[2] == 'C' && d[3] == 'O' && d[4] == 'M';
}

// PC-1600 header: 0x00..0x03 = FF 10 00 00.
bool looksLikePc1600(const uint8_t* d, size_t len) {
    return len >= 16 && d[0] == 0xFF && d[1] == 0x10 && d[2] == 0x00 && d[3] == 0x00;
}

}  // namespace

BasicBinaryImage parseBasicBinaryTransfer(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) return fail("empty transfer file");

    if (looksLikeCe158(data, len)) {
        // Type byte 0x01: 0x40 '@' = tokenized BASIC; 0x41/0x42/0x48 are
        // RESERVE / MACHINE / VARIABLES.
        if (data[1] != 0x40) {
            return fail("not a tokenized BASIC transfer file (CE-158 type byte " + hex2(data[1]) +
                        ", expected 0x40)");
        }
        // Length field at 0x17..0x18, big-endian, capacity-1 encoded.
        size_t payloadLen = (static_cast<size_t>(data[0x17]) << 8 | data[0x18]) + 1;
        size_t expect = 27 + payloadLen;
        if (expect != len) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "CE-158 length field says %zu payload bytes (file is %zu, header 27 + %zu)",
                          payloadLen, len, len - 27);
            return fail(buf);
        }
        BasicBinaryImage img;
        img.ok = true;
        img.model = TransferModel::PC1500;
        img.payload.assign(data + 27, data + 27 + payloadLen);
        return img;
    }

    if (looksLikePc1600(data, len)) {
        // Type byte at 0x04: 0x21 = tokenized BASIC; 0x10 = MACHINE.
        if (data[4] != 0x21) {
            return fail("not a tokenized BASIC transfer file (PC-1600 type byte " + hex2(data[4]) +
                        ", expected 0x21)");
        }
        // Trailer 0x0E..0x0F is the end-of-header marker. The reference doc
        // and the captured `.bbin` fixtures have it as `00 0F`; the
        // SharpDataExchangeJava / SharpDataExchange `convert` toolchain
        // emits the byte-swapped `00 F0`. The bytes carry no payload
        // information (length is at 0x05..0x07), so accept either ordering.
        const bool trailerOk = (data[0x0E] == 0x00 && data[0x0F] == 0x0F) ||
                               (data[0x0E] == 0x00 && data[0x0F] == 0xF0);
        if (!trailerOk) {
            return fail("malformed PC-1600 header (trailer bytes " + hex2(data[0x0E]) + " " +
                        hex2(data[0x0F]) + ", expected 00 0F or 00 F0)");
        }
        // Length field at 0x05..0x07, little-endian, direct count.
        size_t payloadLen = static_cast<size_t>(data[5]) | static_cast<size_t>(data[6]) << 8 |
                            static_cast<size_t>(data[7]) << 16;
        size_t expect = 16 + payloadLen;
        if (expect != len) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "PC-1600 length field says %zu payload bytes (file is %zu, header 16 + %zu)",
                          payloadLen, len, len - 16);
            return fail(buf);
        }
        BasicBinaryImage img;
        img.ok = true;
        img.model = TransferModel::PC1600;
        img.payload.assign(data + 16, data + 16 + payloadLen);
        return img;
    }

    return fail("unrecognized transfer-file header (expected a CE-158 '\\x01..COM' or PC-1600 "
                "'\\xFF\\x10\\x00\\x00' tokenized-BASIC header)");
}

}  // namespace basic
