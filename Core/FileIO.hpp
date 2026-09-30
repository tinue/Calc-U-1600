#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

// Reads the whole file into `out`. False if it can't be opened; the caller
// words its own error.
inline bool readWholeFile(const std::filesystem::path& path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}
