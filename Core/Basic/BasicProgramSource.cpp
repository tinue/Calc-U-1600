#include "BasicProgramSource.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>

#include "vendor/sharpdx/sharpdx.h"

namespace basic {

BasicProgramSource readBasicProgramSource(const std::string& path, TransferModel model) {
    BasicProgramSource s;

    errno = 0;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        s.error = "failed to open program file: " + path + " (" + std::strerror(errno) + ")";
        return s;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        s.error = "program file is empty: " + path;
        return s;
    }

    SdeDevice dev = (model == TransferModel::PC1600) ? SDE_DEVICE_PC1600 : SDE_DEVICE_PC1500;
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
    int32_t rc = sde_tokenize(dev, /*with_header=*/0, SDE_SEGMENT_MARKER_MEMORY, /*name=*/nullptr,
                              bytes.data(), bytes.size(), &out, &out_len);
    if (rc != SDE_OK) {
        std::string msg = sde_last_error();
        if (msg.empty()) msg = "tokenizer error " + std::to_string(rc);
        s.error = "tokenizing " + path + " failed: " + msg;
        return s;
    }
    s.ok = true;
    s.payload.assign(out, out + out_len);
    sde_buf_free(out, out_len);
    return s;
}

}  // namespace basic
