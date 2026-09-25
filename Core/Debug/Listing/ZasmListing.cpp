#include <cctype>
#include <regex>

#include "Listing.hpp"

// zasm listings (`zasm -uwy src.asm src.lst src.bin`):
//
//   C0EE: CD6601   [17]     call KEYGET          code: address, bytes, T-states, source
//   C0CA: 01020304          after: db 1,2,...    data
//   C0CE: 05060708                               more bytes of the line above
//                           ; a comment          no code: 24 spaces, source
//   ...
//   ; +++ global symbols +++
//   start     = $C0C5 = 49349  CODE    pc1600-rom-dumper.asm:86
//
// No line numbers are printed and included files appear inline, so lines
// are placed by walking the sources. The symbol table's file:line entries
// are recorded as symbols (the labels) -- the walk itself is verified by
// the tests against them.

namespace debug {

namespace {

bool hex4(const std::string& s, size_t pos, uint16_t* v) {
    if (pos + 4 > s.size()) return false;
    uint32_t r = 0;
    for (size_t i = pos; i < pos + 4; i++) {
        const char c = s[i];
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        r = r * 16 + uint32_t(std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : std::toupper(c) - 'A' + 10);
    }
    *v = uint16_t(r);
    return true;
}

} // namespace

bool detectZasm(const std::vector<std::string>& text) {
    for (size_t i = 0; i < text.size() && i < 10; i++)
        if (text[i].find("; zasm: assemble") != std::string::npos) return true;
    return false;
}

bool parseZasm(const ListingInput& in, Listing* out, std::string* error) {
    static const std::regex kAssemble(R"re(; zasm: assemble "([^"]+)")re");
    static const std::regex kSymbol(R"(^(\S+)\s*=\s*\$([0-9A-Fa-f]+)\s*=\s*-?\d+)");

    std::string main = in.source;
    size_t first = 0;
    for (size_t i = 0; i < in.text.size() && i < 10; i++) {
        std::smatch m;
        if (std::regex_search(in.text[i], m, kAssemble)) {
            if (main.empty()) main = absolutePath(m[1].str(), directoryOf(in.path));
            first = i + 3; // "; zasm: assemble", "; date:", "; ------" -- the header ends here
            break;
        }
    }
    if (main.empty()) main = in.path.substr(0, in.path.find_last_of('.')) + ".asm";

    SourceWalker walker(*out, in.reader);
    walker.begin(main);
    bool symbols = false;
    bool lastWasCode = false;
    for (size_t i = first; i < in.text.size(); i++) {
        std::string line = in.text[i];
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("; +++ ", 0) == 0) { symbols = true; continue; }
        if (symbols) {
            std::smatch m;
            if (std::regex_search(line, m, kSymbol))
                out->symbols[m[1].str()] = uint16_t(std::stoul(m[2].str(), nullptr, 16));
            continue;
        }
        uint16_t addr = 0;
        const bool hasAddr = line.size() >= 5 && line[4] == ':' && hex4(line, 0, &addr);
        const std::string text = line.size() > 24 ? line.substr(24) : std::string();
        std::vector<uint8_t> bytes;
        if (hasAddr) {
            const std::string hex = line.size() > 6 ? line.substr(6, 8) : std::string();
            for (size_t j = 0; j + 1 < hex.size() && std::isxdigit(static_cast<unsigned char>(hex[j])) &&
                               std::isxdigit(static_cast<unsigned char>(hex[j + 1]));
                 j += 2)
                bytes.push_back(uint8_t(std::stoi(hex.substr(j, 2), nullptr, 16)));
        }
        if (hasAddr && !bytes.empty() && normalizeSourceText(text).empty() && lastWasCode) {
            out->lines.back().bytes.insert(out->lines.back().bytes.end(), bytes.begin(), bytes.end());
            continue;
        }
        int file = 0, srcLine = 0;
        lastWasCode = false;
        if (!walker.place(text, 0, &file, &srcLine)) continue;
        if (hasAddr && !bytes.empty()) {
            out->lines.push_back({addr, std::move(bytes), file, srcLine});
            lastWasCode = true;
        }
    }
    if (walker.unmatched() > 0)
        out->warnings.push_back(std::to_string(walker.unmatched()) + " listing lines didn't match their source text");
    if (out->lines.empty() && out->symbols.empty()) {
        if (error) *error = "no code or symbols in " + in.path;
        return false;
    }
    return true;
}

} // namespace debug
