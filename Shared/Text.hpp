#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

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

// The lines, one per line (no trailing line break).
inline std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) out += '\n';
        out += lines[i];
    }
    return out;
}

// `s` in lower case (ASCII).
inline std::string lowered(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Equal ignoring ASCII case, the way names and file suffixes compare.
inline bool sameIgnoringCase(const std::string& a, const std::string& b) { return lowered(a) == lowered(b); }

// `s` ends with `tail`, ignoring ASCII case.
inline bool endsWithIgnoringCase(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && sameIgnoringCase(s.substr(s.size() - tail.size()), tail);
}

} // namespace shared
