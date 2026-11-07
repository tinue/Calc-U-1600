#pragma once
#include <filesystem>
#include <string>

// Shared/ keeps every path as a UTF-8 std::string (what QString::toStdString
// and a browser give). std::filesystem::path(std::string) reads the string
// in the ANSI code page on Windows, so Shared converts through these, via
// char8_t (C++20: u8path is deprecated, u8string() returns std::u8string).
namespace shared {

inline std::filesystem::path toPath(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

inline std::string fromPath(const std::filesystem::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

} // namespace shared
