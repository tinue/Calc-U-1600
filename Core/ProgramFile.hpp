#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ── What a program file holds ──────────────────────────────────────────
//
// The one place that asks libsharpdx (sde_file_info) what a file is: a
// BASIC listing, tokenized BASIC or machine code behind a CE-158 or
// PC-1600 header, or headerless bytes -- or a cassette WAV, which it decodes
// (sde_wav_decode) and then describes as the file on the tape. Every loader builds on it --
// machinecode::readFile() and basic::readBasicProgram(). Header layouts:
// Ref/Shared/Data-Formats/Binary-Exchange-Formats.md §2 / §3.
//
// No CPU guessing (docs/background/plans/Loader-Mode-Plan.md, principle 3):
// the library's raw-lh5801 / raw-z80 guess for headerless code never picks
// the CPU -- all of them are Headerless, and the MODE (or the debugger's
// `cpu`) decides.
// The guess survives only as `looksLikeCode`, which decides whether a
// dropped file is accepted at all (Core/DropFile).

namespace programfile {

enum class Kind {
    Empty,
    BasicListing,  // ASCII BASIC listing (.bas)
    BasicPC1500,   // tokenized BASIC behind a CE-158 header
    BasicPC1600,   // tokenized BASIC behind a PC-1600 header
    CodeLH5801,    // machine code behind a CE-158 header
    CodeZ80,       // machine code behind a PC-1600 header
    Headerless,    // anything else binary: machine code without a header
    Other,         // Reserve Area, Variables, plain text
};

struct ProgramFile {
    Kind kind = Kind::Empty;
    std::string token;  // sde_file_info's kind token ("reserve", "text", ...), for messages
    /// Header magic without a complete header of a known type, or a payload
    /// that doesn't decode (tokenized BASIC that doesn't de-tokenize, ...).
    bool damaged = false;
    /// The header's length disagrees with the bytes that follow it:
    /// `truncated` (fewer) or trailing bytes (more).
    bool lengthMismatch = false;
    bool truncated = false;
    /// Headerless bytes that the library's heuristic takes for LH5801 or Z80
    /// code (raw-lh5801 / raw-z80). Never used to pick the CPU.
    bool looksLikeCode = false;
    /// Everything after the header, to the end of the file -- not cut to the
    /// header's length, so a preset's `length:` can override a mismatch.
    /// Headerless: the whole file.
    std::vector<uint8_t> payload;
    size_t headerPayloadLen = 0;  // the payload the header describes (== payload.size() unless mismatched)
    uint32_t loadAddr = 0;        // code: load address (PC-1600: bank in bits 16-23)
    uint32_t autorunAddr = 0;     // code: auto-run address; 0 = none (the header held 0 or FFFF)
    /// A cassette WAV (PC-1500 CE-150 / PC-1600 CE-1600P tape). Everything
    /// above then describes the first file on it that decodes safely; with
    /// none, the kind is Other and `token` says why.
    bool fromTape = false;
};

ProgramFile classify(const std::vector<uint8_t>& bytes);

/// One file of a cassette WAV, as the serial image (CE-158 / PC-1600 header +
/// payload) every loader takes.
struct TapeFile {
    std::string name;
    std::vector<uint8_t> image;
};

/// The files of a cassette WAV that decode safely, in tape order. Empty when
/// `bytes` is no tape or nothing on it decodes; `*error` (may be null) then
/// says why.
std::vector<TapeFile> tapeFiles(const std::vector<uint8_t>& bytes, std::string* error);

/// A Kind::Other file's token in words ("plain text", "a Reserve Area as text", ...).
std::string describe(const std::string& token);

/// "CE-158" or "PC-1600" for a headered kind.
inline const char* headerName(Kind kind) {
    return kind == Kind::BasicPC1600 || kind == Kind::CodeZ80 ? "PC-1600" : "CE-158";
}

}  // namespace programfile
