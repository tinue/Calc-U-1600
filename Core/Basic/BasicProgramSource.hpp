#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ── A BASIC program file, as the fast loaders take it ──────────────────
//
// Load BASIC Program… and a preset's a BASIC `program: file:` read either
// file kind programfile::classify() (libsharpdx) recognises as BASIC:
//
//   * a plain-text listing (.bas) -- tokenized in-process via
//     sde_tokenize with `listingModel`'s keyword table and content rules
//     (notably the PC-1500 7-bit-ASCII check, which refuses a listing with
//     a non-ASCII character);
//   * tokenized BASIC behind a CE-158 (PC-1500) or PC-1600 header (.bbin,
//     as SharpDataExchange writes it) -- the header is stripped.
//
// Either way the result is the run of in-RAM line records the loaders poke
// into the program area:
//
//   [lineNo hi][lineNo lo][len][content ...][0x0D]   len = content + 1
//
// big-endian line number, 2-byte keyword tokens, no next-address link and
// no program-end marker (the loader appends the model's 0xFF itself). Which
// machine may take which `source` is the loader's call.

namespace basic {

enum class TransferModel { PC1500, PC1600 };

struct BasicProgramSource {
    bool ok = false;
    TransferModel source = TransferModel::PC1500;  // the header's machine, or listingModel for a listing
    bool listing = false;                          // a .bas listing (tokenized here, or refused by the tokenizer)
    std::vector<uint8_t> payload;                  // bare tokenized line records (no header)
    std::string error;                             // set when ok == false
};

BasicProgramSource readBasicProgram(const std::vector<uint8_t>& bytes, TransferModel listingModel);

/// Same, for the file at `path`; errors name it.
BasicProgramSource readBasicProgramFile(const std::string& path, TransferModel listingModel);

}  // namespace basic
