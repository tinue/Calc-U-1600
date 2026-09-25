#include "Listing.hpp"

#include "../../HexFormat.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace debug {

namespace fs = std::filesystem;

SourceReader diskReader() {
    return [](const std::string& path, std::vector<std::string>* lines) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        lines->clear();
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines->push_back(line);
        }
        return true;
    };
}

std::string absolutePath(const std::string& path, const std::string& baseDir) {
    fs::path p(path);
    if (p.is_relative()) p = (baseDir.empty() ? fs::current_path() : fs::path(baseDir)) / p;
    return p.lexically_normal().string();
}

std::string directoryOf(const std::string& path) {
    return fs::path(path).parent_path().string();
}

std::string normalizeSourceText(const std::string& text) {
    std::string out;
    bool space = false;
    for (char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) { space = !out.empty(); continue; }
        if (space) { out += ' '; space = false; }
        out += c;
    }
    return out;
}

std::string includeTarget(const std::string& text) {
    const std::string t = normalizeSourceText(text);
    std::string lower;
    for (char c : t) lower += char(std::tolower(static_cast<unsigned char>(c)));
    // A label may precede the directive in some dialects; look for the keyword.
    size_t kw = std::string::npos;
    for (const char* k : {".include ", "#include ", "include "}) {
        const size_t at = lower.find(k);
        if (at != std::string::npos && (at == 0 || lower[at - 1] == ' ' || lower[at - 1] == ':')) { kw = at; break; }
    }
    if (kw == std::string::npos) return {};
    // Comments before the keyword mean it's no directive.
    const size_t semi = t.find(';');
    if (semi != std::string::npos && semi < kw) return {};
    const size_t q1 = t.find_first_of("\"'<", kw);
    if (q1 == std::string::npos) return {};
    const char close = t[q1] == '<' ? '>' : t[q1];
    const size_t q2 = t.find(close, q1 + 1);
    if (q2 == std::string::npos) return {};
    return t.substr(q1 + 1, q2 - q1 - 1);
}

// ── SourceWalker ──────────────────────────────────────────────────────────

int SourceWalker::fileIndex(const std::string& path) {
    for (size_t i = 0; i < m_listing.files.size(); i++)
        if (m_listing.files[i] == path) return int(i);
    m_listing.files.push_back(path);
    return int(m_listing.files.size() - 1);
}

void SourceWalker::push(const std::string& path) {
    Open f;
    f.file = fileIndex(path);
    if (!m_reader || !m_reader(path, &f.lines)) {
        m_listing.warnings.push_back("can't read source " + path);
        f.lines.clear();
    }
    m_stack.push_back(std::move(f));
}

void SourceWalker::begin(const std::string& mainFile) {
    m_stack.clear();
    push(mainFile);
}

bool SourceWalker::matches(const Open& f, int index, const std::string& text) const {
    if (index < 0 || index >= int(f.lines.size())) return false;
    const std::string a = normalizeSourceText(text);
    const std::string b = normalizeSourceText(f.lines[size_t(index)]);
    if (a == b) return true;
    // A listing may cut long source lines short.
    return a.size() >= 40 && b.compare(0, a.size(), a) == 0;
}

void SourceWalker::noteInclude(const std::string& text, const std::string& includerDir) {
    const std::string target = includeTarget(text);
    if (!target.empty()) push(absolutePath(target, includerDir));
}

bool SourceWalker::place(const std::string& text, int printedLine, int* file, int* line) {
    if (m_stack.empty()) return false;
    auto accept = [&](size_t depth, int index) {
        m_stack.resize(depth + 1);
        Open& f = m_stack.back();
        f.cursor = index + 1;
        *file = f.file;
        *line = index + 1;
        noteInclude(text, directoryOf(m_listing.files[size_t(f.file)]));
        return true;
    };

    if (printedLine > 0) {
        // The assembler numbered the line: the innermost open file whose
        // text agrees owns it.
        for (size_t d = m_stack.size(); d-- > 0;)
            if (matches(m_stack[d], printedLine - 1, text)) return accept(d, printedLine - 1);
        // No source to confirm (unreadable file, macro expansion text):
        // trust the number and the innermost file.
        m_unmatched++;
        return accept(m_stack.size() - 1, printedLine - 1);
    }

    // No line numbers: follow each file's cursor.
    if (normalizeSourceText(text).empty()) {
        Open& top = m_stack.back();
        if (top.cursor < int(top.lines.size()) && normalizeSourceText(top.lines[size_t(top.cursor)]).empty())
            return accept(m_stack.size() - 1, top.cursor);
        return false; // listing header/trailer spacing
    }
    constexpr int kLookahead = 64; // lines the assembler may leave out (conditionals, listing off)
    for (size_t d = m_stack.size(); d-- > 0;) {
        const Open& f = m_stack[d];
        const int end = std::min(int(f.lines.size()), f.cursor + kLookahead);
        for (int i = f.cursor; i < end; i++)
            if (matches(f, i, text)) return accept(d, i);
    }
    m_unmatched++;
    return false;
}

// ── Loading ───────────────────────────────────────────────────────────────

const std::vector<ListingFormat>& listingFormats() {
    static const std::vector<ListingFormat> kFormats = {
        {"zasm", detectZasm, parseZasm},
        {"sdas", detectSdas, parseSdas},
    };
    return kFormats;
}

bool loadListing(const std::string& path, Listing* out, std::string* error, const std::string& source, SourceReader reader) {
    ListingInput in;
    in.path = absolutePath(path);
    in.source = source.empty() ? std::string() : absolutePath(source);
    in.reader = reader;
    if (!reader(in.path, &in.text)) {
        if (error) *error = "can't read listing " + in.path;
        return false;
    }
    for (const ListingFormat& f : listingFormats()) {
        if (!f.detect(in.text)) continue;
        *out = Listing();
        out->format = f.name;
        return f.parse(in, out, error);
    }
    if (error) *error = "unrecognised listing format: " + in.path;
    return false;
}

bool loadSymbolFile(const std::string& path, Listing* out, std::string* error, SourceReader reader) {
    std::vector<std::string> text;
    const std::string abs = absolutePath(path);
    if (!reader(abs, &text)) {
        if (error) *error = "can't read symbol file " + abs;
        return false;
    }
    *out = Listing();
    out->format = "symbols";
    bool inTable = false;
    for (const std::string& raw : text) {
        const std::string t = normalizeSourceText(raw);
        if (t.empty() || t[0] == ';') continue;
        if (t == ".SYMBOLS:" || t == ".symbols:") { inTable = true; continue; }
        if (!inTable) continue;
        const size_t sp = t.find(' ');
        if (sp != 4) continue;
        uint32_t v = 0;
        if (!parseHexField(t, 0, 4, &v)) continue;
        std::string name = t.substr(5);
        const size_t end = name.find_first_of(" ;");
        if (end != std::string::npos) name.resize(end);
        if (!name.empty()) out->symbols[name] = uint16_t(v);
    }
    if (out->symbols.empty()) {
        if (error) *error = "no .SYMBOLS: entries in " + abs;
        return false;
    }
    return true;
}

bool loadListingWithSymbols(const std::string& listingPath, const std::string& source,
                            const std::vector<std::string>& symbolFiles, Listing* out,
                            std::vector<std::string>* warnings, SourceReader reader) {
    std::string err;
    *out = Listing();
    bool loaded = false;
    if (!listingPath.empty()) {
        if (!loadListing(listingPath, out, &err, source, reader)) {
            warnings->push_back("listing not loaded: " + err);
            return false;
        }
        for (const std::string& w : out->warnings) warnings->push_back(listingPath + ": " + w);
        loaded = true;
    } else {
        out->format = "symbols";
    }
    for (const std::string& path : symbolFiles) {
        Listing symbols;
        if (loadSymbolFile(path, &symbols, &err, reader)) {
            out->symbols.insert(symbols.symbols.begin(), symbols.symbols.end());
            loaded = true;
        } else {
            warnings->push_back(err);
        }
    }
    return loaded;
}

} // namespace debug
