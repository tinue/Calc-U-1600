#include "BasicProgramSource.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>

#include "../ProgramFile.hpp"
#include "vendor/sharpdx/sharpdx.h"

namespace basic {

namespace {

BasicProgramSource fail(const std::string& msg) {
    BasicProgramSource s;
    s.error = msg;
    return s;
}

BasicProgramSource tokenize(const std::vector<uint8_t>& bytes, TransferModel model) {
    const SdeDevice dev = (model == TransferModel::PC1600) ? SDE_DEVICE_PC1600 : SDE_DEVICE_PC1500;
    uint8_t* out = nullptr;
    size_t out_len = 0;
    // SDE_SEGMENT_MARKER_MEMORY: a `#SEGMENT` line (two independently
    // line-numbered programs concatenated so one can GOSUB "LABEL" into the
    // other) tokenizes to the bare 0xFF the ROM's serial receiver actually
    // stores in the program area, not the 3-byte 0xFF 0x00 0x00 wire form
    // SAVE "COM1:" transmits (that trailing 0x00 0x00 is a transmission-only
    // pacing marker -- see SharpDataExchange's sender.rs/scanner.rs).
    // This is a direct-poke loader, not a serial transfer, so it wants what
    // actually ends up in RAM.
    const int32_t rc = sde_tokenize(dev, /*with_header=*/0, SDE_SEGMENT_MARKER_MEMORY, /*name=*/nullptr,
                                    bytes.data(), bytes.size(), &out, &out_len);
    if (rc != SDE_OK) {
        std::string msg = sde_last_error();
        if (msg.empty()) msg = "tokenizer error " + std::to_string(rc);
        BasicProgramSource s = fail("tokenizing failed: " + msg);
        s.listing = true;
        return s;
    }
    BasicProgramSource s;
    s.ok = true;
    s.source = model;
    s.listing = true;
    s.payload.assign(out, out + out_len);
    sde_buf_free(out, out_len);
    return s;
}

}  // namespace

BasicProgramSource readBasicProgram(const std::vector<uint8_t>& bytes, TransferModel listingModel) {
    using programfile::Kind;
    const programfile::ProgramFile pf = programfile::classify(bytes);
    switch (pf.kind) {
        case Kind::Empty:
            return fail("the file is empty");
        case Kind::BasicListing:
            return tokenize(bytes, listingModel);
        case Kind::BasicPC1500:
        case Kind::BasicPC1600:
            break;
        case Kind::CodeLH5801:
        case Kind::CodeZ80:
            return fail(std::string("this is machine code (") + programfile::headerName(pf.kind) +
                        " header), not a BASIC program -- use Load Machine Code");
        case Kind::Headerless:
            if (pf.damaged)
                return fail("the file starts like a CE-158 or PC-1600 header, but has no complete header of a "
                            "known type");
            return fail("this is neither a BASIC listing nor tokenized BASIC with a CE-158 or PC-1600 header");
        case Kind::Other:
            return fail("this file holds " + programfile::describe(pf.token) + ", not a BASIC program");
    }
    const char* header = programfile::headerName(pf.kind);
    if (pf.truncated)
        return fail(std::string("the ") + header + " header promises more than the " +
                    std::to_string(pf.payload.size()) + " bytes that follow it");
    if (pf.lengthMismatch)
        return fail(std::string("the ") + header + " header says " + std::to_string(pf.headerPayloadLen) +
                    " bytes of program, but " + std::to_string(pf.payload.size()) + " follow it");
    if (pf.damaged) return fail(std::string("the tokenized BASIC behind the ") + header + " header is damaged");
    BasicProgramSource s;
    s.ok = true;
    s.source = pf.kind == Kind::BasicPC1600 ? TransferModel::PC1600 : TransferModel::PC1500;
    s.payload = pf.payload;
    return s;
}

BasicProgramSource readBasicProgramFile(const std::string& path, TransferModel listingModel) {
    errno = 0;
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("failed to open program file: " + path + " (" + std::strerror(errno) + ")");
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    BasicProgramSource s = readBasicProgram(bytes, listingModel);
    if (!s.ok) s.error = path + ": " + s.error;
    return s;
}

}  // namespace basic
