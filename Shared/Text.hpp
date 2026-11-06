#pragma once
#include <string>

// Small string helpers for the bays' names and messages.
namespace shared {

// `s` without leading/trailing spaces, tabs and line breaks.
inline std::string trimmed(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// `s` in double quotes, the way messages name a card, disk or file.
inline std::string inQuotes(const std::string& s) { return "\"" + s + "\""; }

} // namespace shared
