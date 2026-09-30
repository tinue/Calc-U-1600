#include "DropFile.hpp"

#include <cctype>
#include <cstring>

#include "ProgramFile.hpp"
#include "Utf8.hpp"

namespace dropfile {

namespace {

bool validUtf8(std::string_view text) {
    char32_t cp;
    for (size_t i = 0; i < text.size();)
        if (!decodeUtf8(text, i, cp)) return false;
    return true;
}

// NUL-free UTF-8 with a line that starts with `model:` in column 0 --
// indented or commented-out keys don't count.
bool looksLikePreset(const std::vector<uint8_t>& bytes) {
    const uint8_t* p = bytes.data();
    size_t n = bytes.size();
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; n -= 3; }
    static const char kKey[] = "model:";
    const size_t keyLen = sizeof(kKey) - 1;
    for (size_t line = 0; line < n;) {
        // The line search first: a binary rarely has one, so it exits early.
        if (n - line >= keyLen && std::memcmp(p + line, kKey, keyLen) == 0)
            return std::memchr(p, 0, n) == nullptr && validUtf8({reinterpret_cast<const char*>(p), n});
        const void* nl = std::memchr(p + line, '\n', n - line);
        if (nl == nullptr) break;
        line = static_cast<size_t>(static_cast<const uint8_t*>(nl) - p) + 1;
    }
    return false;
}

bool codeFileName(const std::string& fileName) {
    const size_t dot = fileName.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = fileName.substr(dot + 1);
    for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return ext == "bin" || ext == "rom";
}

}  // namespace

Target classify(const std::vector<uint8_t>& bytes, const std::string& fileName) {
    if (looksLikePreset(bytes)) return Target::Preset;
    using programfile::Kind;
    const programfile::ProgramFile f = programfile::classify(bytes);
    switch (f.kind) {
        case Kind::BasicListing:
        case Kind::BasicPC1500:
        case Kind::BasicPC1600: return Target::BasicProgram;
        case Kind::CodeLH5801:
        case Kind::CodeZ80: return Target::MachineCode;
        case Kind::Headerless:
            return f.looksLikeCode && codeFileName(fileName) ? Target::MachineCode : Target::None;
        case Kind::Empty:
        case Kind::Other: return Target::None;
    }
    return Target::None;
}

}  // namespace dropfile
