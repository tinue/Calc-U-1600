#include <cctype>
#include <regex>

#include "Listing.hpp"

// sdas (ASxxxx) listings: sdaslh5801 / sdasz80 `.lst`, and the `.rst` the
// linker rewrites with relocated addresses and bytes. Fixed columns, in
// one of two layouts. sdaslh5801 prints 16-bit addresses:
//
//   0         1         2         3
//   0123456789012345678901234567890123
//      40CE AE 40 C7             90             sta     (ERR_FLAG)
//                        7867    63 BOTTOM_H    .equ    0x7867
//           07 08 09 0A 0B 0C                    (continuation: more bytes)
//
// [3,7) address, [8,25) up to six bytes (relocatable ones carry r/s/R/S
// markers between them in a .lst), [21,25) an equate's value, [25,31) the
// source line number, [32,...) the source text. sdasz80 prints 32-bit
// addresses and a T-state column:
//
//       0000C0CB CD D1 C0         [17]   12 	call	delay
//                            00001234     1 VAL = 0x1234
//
// [4,12) address, [13,34) up to seven bytes (the "[17]" cycles sit at the
// end of that field), [25,33) an equate's value, [34,39) the line number,
// [40,...) the source. Either way an included file's lines restart the
// numbering and aren't otherwise marked.

namespace debug {

namespace {

std::string field(const std::string& s, size_t pos, size_t len) {
    return pos < s.size() ? s.substr(pos, len) : std::string();
}

struct Layout {
    size_t addr, addrLen, bytes, bytesLen, value, valueLen, lineNo, source;
};
constexpr Layout k16 = {3, 4, 8, 17, 21, 4, 25, 32};
constexpr Layout k32 = {4, 8, 13, 21, 25, 8, 34, 40};

const Layout& layoutOf(const std::vector<std::string>& text) {
    for (size_t i = 0; i < text.size() && i < 400; i++) {
        const std::string& l = text[i];
        if (l.size() > 13 && l.compare(0, 4, "    ") == 0 && l[12] == ' ') {
            bool hex = true;
            for (size_t k = 4; k < 12 && hex; k++) hex = std::isxdigit(static_cast<unsigned char>(l[k])) != 0;
            if (hex) return k32;
        }
    }
    return k16;
}

// The low 16 bits of a 4- or 8-digit hex field.
bool parseHex16(const std::string& s, uint16_t* v) {
    if (s.size() != 4 && s.size() != 8) return false;
    uint32_t r = 0;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        r = r * 16 + uint32_t(std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : std::toupper(c) - 'A' + 10);
    }
    *v = uint16_t(r);
    return true;
}

std::vector<uint8_t> hexBytes(std::string s) {
    s = s.substr(0, s.find('[')); // sdasz80's T-state column
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size();) {
        if (std::isxdigit(static_cast<unsigned char>(s[i])) && std::isxdigit(static_cast<unsigned char>(s[i + 1]))) {
            out.push_back(uint8_t(std::stoi(s.substr(i, 2), nullptr, 16)));
            i += 2;
        } else {
            i++;
        }
    }
    return out;
}

bool digitsOnly(const std::string& s, int* value) {
    std::string t;
    for (char c : s) if (c != ' ') t += c;
    if (t.empty()) return false;
    for (char c : t) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    *value = std::stoi(t);
    return true;
}

bool pageHeader(const std::string& line) {
    return line.find("ASxxxx Assembler") != std::string::npos || line.rfind("Hexadecimal [", 0) == 0 ||
           line.rfind("Octal [", 0) == 0 || line.rfind("Decimal [", 0) == 0 || (!line.empty() && line[0] == '\f');
}

} // namespace

bool detectSdas(const std::vector<std::string>& text) {
    const Layout& lay = layoutOf(text);
    int numbered = 0;
    for (size_t i = 0; i < text.size() && i < 200; i++) {
        const std::string& l = text[i];
        if (l.find("ASxxxx Assembler") != std::string::npos) return true;
        int n;
        if (l.size() >= lay.lineNo + 6 && l.compare(0, 3, "   ") == 0 && digitsOnly(field(l, lay.lineNo, lay.source - 1 - lay.lineNo), &n)) numbered++;
    }
    return numbered >= 3;
}

bool parseSdas(const ListingInput& in, Listing* out, std::string* error) {
    std::string main = in.source;
    if (main.empty()) {
        // memtest.rst / memtest.lst -> memtest.asm (or .s)
        const std::string stem = in.path.substr(0, in.path.find_last_of('.'));
        std::vector<std::string> probe;
        main = stem + ".asm";
        if (in.reader && !in.reader(main, &probe) && in.reader(stem + ".s", &probe)) main = stem + ".s";
    }
    SourceWalker walker(*out, in.reader);
    walker.begin(main);
    const Layout& lay = layoutOf(in.text);

    static const std::regex kLabel(R"(^([A-Za-z_.][A-Za-z0-9_.]*)::?)");
    static const std::regex kEquate(R"(^\s*([A-Za-z_.][A-Za-z0-9_.]*)\s*(==?|\.equ|\.gblequ|\.lclequ)(\s|$))",
                                    std::regex::icase);
    bool lastWasCode = false;
    for (std::string line : in.text) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("Symbol Table", 0) == 0 || line.rfind("Area Table", 0) == 0) break;
        if (pageHeader(line) || line.size() < 8) { lastWasCode = false; continue; }

        uint16_t addr = 0;
        const bool hasAddr = parseHex16(field(line, lay.addr, lay.addrLen), &addr);
        int lineNo = 0;
        const bool numbered = digitsOnly(field(line, lay.lineNo, lay.source - 1 - lay.lineNo), &lineNo);
        if (!numbered) {
            // More bytes of the previous line's data.
            const std::vector<uint8_t> more = hexBytes(field(line, lay.bytes, lay.bytesLen));
            if (!hasAddr && lastWasCode && !more.empty())
                out->lines.back().bytes.insert(out->lines.back().bytes.end(), more.begin(), more.end());
            continue;
        }
        const std::string text = field(line, lay.source, std::string::npos);
        int file = 0, srcLine = 0;
        lastWasCode = false;
        if (!walker.place(text, lineNo, &file, &srcLine)) continue;

        std::smatch m;
        if (hasAddr) {
            if (std::regex_search(text, m, kLabel) && m[1].str().find('$') == std::string::npos)
                out->symbols[m[1].str()] = addr;
            std::vector<uint8_t> bytes = hexBytes(field(line, lay.bytes, lay.bytesLen));
            if (!bytes.empty()) {
                out->lines.push_back({addr, std::move(bytes), file, srcLine});
                lastWasCode = true;
            }
        } else if (std::regex_search(text, m, kEquate)) {
            uint16_t value;
            if (parseHex16(field(line, lay.value, lay.valueLen), &value)) out->symbols[m[1].str()] = value;
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
