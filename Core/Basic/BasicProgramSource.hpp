#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "BasicBinaryImage.hpp"  // basic::TransferModel

// ── `format: basic-binary` program source ──────────────────────────────
//
// A `program: format: basic-binary` preset section names a `path:` to a
// plain-text BASIC listing. This tokenizes it in-process via the vendored
// `libsharpdx` (`convert` verb, headerless) into the run of in-RAM line
// records `loadBasicBinaryPayload` pokes into the program area.
//
// The target `model` is passed through as the tokenizer's device: it
// selects the keyword table *and* the content rules -- notably the PC-1500
// 7-bit-ASCII check, which rejects a listing with a non-ASCII character
// before it is loaded.

namespace basic {

struct BasicProgramSource {
    bool ok = false;
    std::vector<uint8_t> payload;  // bare tokenized line records (no header)
    std::string error;             // set when ok == false
};

BasicProgramSource readBasicProgramSource(const std::string& path, TransferModel model);

}  // namespace basic
