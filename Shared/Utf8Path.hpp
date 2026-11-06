#pragma once
#include <filesystem>
#include <string>

// Shared/ keeps every path as a UTF-8 std::string (what QString::toStdString
// and a browser give). std::filesystem::path(std::string) reads the string
// in the ANSI code page on Windows, so Shared converts through these.
// (C++17: u8path / generic_u8string return std::string.)
namespace shared {

inline std::filesystem::path toPath(const std::string& utf8) { return std::filesystem::u8path(utf8); }
inline std::string fromPath(const std::filesystem::path& p) { return p.generic_u8string(); }

} // namespace shared
