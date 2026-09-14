#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ── SharpDataExchange "tokenized BASIC" transfer-file parser ────────────
//
// Shared core helper (no machine / GUI dependency) for the fast BASIC
// program loader: it turns a pre-tokenized transfer file -- the kind
// SharpDataExchange emits with `put --dry-run` / a captured serial
// transfer -- into the raw payload bytes that get poked into the BASIC
// program area.
//
// The payload is a run of in-RAM line records, exactly the layout the
// PC-1500/PC-1600 ROM line editor produces (verified against the
// `--dump-basic` oracle):
//
//   [lineNo hi][lineNo lo][len][content ...][0x0D]   len = content + 1
//
// big-endian line number, 2-byte big-endian keyword tokens, no
// next-address link, and **no program-end marker** (the loader appends the
// model's 0xFF marker itself). Two header shapes are accepted, per
// SharpPC1500Reference/Data-Formats/Binary-Exchange-Formats.md:
//
//   * CE-158  (PC-1500 / PC-1500A): 27 bytes, magic 0x01 .. "COM",
//     type byte 0x40 ('@'), length field is capacity-1 encoded.
//   * PC-1600: 16 bytes, magic FF 10 00 00, type byte 0x21, length is a
//     direct 3-byte little-endian count, trailer 00 0F.
//
// Anything else -- a headerless payload, a MACHINE / RESERVE / VARIABLES
// transfer, a length field inconsistent with the file size -- is rejected
// with a specific message.

namespace basic {

enum class TransferModel { PC1500, PC1600 };

struct BasicBinaryImage {
    bool ok = false;
    TransferModel model = TransferModel::PC1500;
    std::vector<uint8_t> payload;  // tokenized line records, header stripped
    std::string error;             // set when ok == false
};

BasicBinaryImage parseBasicBinaryTransfer(const uint8_t* data, size_t len);

inline BasicBinaryImage parseBasicBinaryTransfer(const std::vector<uint8_t>& bytes) {
    return parseBasicBinaryTransfer(bytes.data(), bytes.size());
}

}  // namespace basic
