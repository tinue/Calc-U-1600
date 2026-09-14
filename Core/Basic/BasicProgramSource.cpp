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
    int32_t rc = sde_tokenize(dev, /*with_header=*/0, /*name=*/nullptr, bytes.data(), bytes.size(),
                              &out, &out_len);
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
