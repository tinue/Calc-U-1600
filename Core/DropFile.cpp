#include "DropFile.hpp"

#include <cstring>

#include "ProgramFile.hpp"

namespace dropfile {

namespace {

// Well-formed UTF-8 (no overlongs, surrogates or code points past U+10FFFF).
bool validUtf8(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        const uint8_t c = p[i];
        size_t len;
        uint32_t cp;
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        static const uint32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

// NUL-free UTF-8 with a line that starts with `model:` in column 0 --
// indented or commented-out keys don't count.
bool looksLikePreset(const std::vector<uint8_t>& bytes) {
    const uint8_t* p = bytes.data();
    size_t n = bytes.size();
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; n -= 3; }
    if (std::memchr(p, 0, n) != nullptr || !validUtf8(p, n)) return false;
    static const char kKey[] = "model:";
    const size_t keyLen = sizeof(kKey) - 1;
    for (size_t line = 0; line < n;) {
        if (n - line >= keyLen && std::memcmp(p + line, kKey, keyLen) == 0) return true;
        const void* nl = std::memchr(p + line, '\n', n - line);
        if (nl == nullptr) break;
        line = static_cast<size_t>(static_cast<const uint8_t*>(nl) - p) + 1;
    }
    return false;
}

}  // namespace

Target classify(const std::vector<uint8_t>& bytes) {
    if (looksLikePreset(bytes)) return Target::Preset;
    using programfile::Kind;
    const programfile::ProgramFile f = programfile::classify(bytes);
    switch (f.kind) {
        case Kind::BasicListing:
        case Kind::BasicPC1500:
        case Kind::BasicPC1600: return Target::BasicProgram;
        case Kind::CodeLH5801:
        case Kind::CodeZ80: return Target::MachineCode;
        case Kind::Headerless: return f.looksLikeCode ? Target::MachineCode : Target::None;
        case Kind::Empty:
        case Kind::Other: return Target::None;
    }
    return Target::None;
}

}  // namespace dropfile
