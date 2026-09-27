#include "ProgramFile.hpp"

#include <cstring>

#include "Basic/vendor/sharpdx/sharpdx.h"

namespace programfile {

namespace {

Kind kindOf(const char* token) {
    struct Entry {
        const char* token;
        Kind kind;
    };
    static const Entry kEntries[] = {
        {"empty", Kind::Empty},
        {"basic-ascii", Kind::BasicListing},
        {"basic-pc1500", Kind::BasicPC1500},
        {"basic-pc1600", Kind::BasicPC1600},
        {"ml-lh5801", Kind::CodeLH5801},
        {"ml-z80", Kind::CodeZ80},
        // The CPU guesses: a headerless file is headerless, whatever it looks like.
        {"raw", Kind::Headerless},
        {"raw-lh5801", Kind::Headerless},
        {"raw-z80", Kind::Headerless},
    };
    for (const Entry& e : kEntries)
        if (std::strcmp(token, e.token) == 0) return e.kind;
    return Kind::Other;  // reserve, reserve-text, variables, variables-text, text
}

}  // namespace

ProgramFile classify(const std::vector<uint8_t>& bytes) {
    ProgramFile f;
    SdeFileInfo fi{};
    if (sde_file_info(bytes.data(), bytes.size(), &fi) != SDE_OK || fi.kind == nullptr) {
        // Only a NULL buffer or a caught panic gets here; neither is a file we can load.
        f.kind = Kind::Other;
        f.token = "unreadable";
        f.damaged = true;
        return f;
    }
    f.kind = kindOf(fi.kind);
    f.token = fi.kind;
    f.damaged = (fi.problems & (SDE_PROBLEM_HEADER_CUT | SDE_PROBLEM_BAD_PAYLOAD)) != 0;
    f.truncated = (fi.problems & SDE_PROBLEM_TRUNCATED) != 0;
    f.lengthMismatch = (fi.problems & (SDE_PROBLEM_TRUNCATED | SDE_PROBLEM_TRAILING)) != 0;
    f.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(fi.payload_offset), bytes.end());
    f.headerPayloadLen = fi.payload_len;
    if (f.kind == Kind::CodeLH5801 || f.kind == Kind::CodeZ80) {
        f.loadAddr = fi.load_addr;
        // The library reads FFFF as "no auto-start"; 0 is none as well.
        f.autorunAddr = fi.autorun && (fi.run_addr & 0xFFFF) != 0 ? fi.run_addr : 0;
    }
    return f;
}

std::string describe(const std::string& token) {
    if (token == "reserve") return "a Reserve Area (CE-158 header)";
    if (token == "reserve-text") return "a Reserve Area as text";
    if (token == "variables") return "variables (CE-158 header)";
    if (token == "variables-text") return "variables as text";
    if (token == "text") return "plain text";
    return token;
}

}  // namespace programfile
