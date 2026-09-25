#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

// ── Assembler listings ───────────────────────────────────────────────────
//
// One parsed listing: every source line that produced bytes, with the file
// and line it came from, plus the symbols it defines. The formats sit
// behind one small table (listingFormats()); each entry recognises its
// assembler's listing and turns it into this model, so adding a format --
// a disassembled ROM listing, a library's -- means adding one parser and
// nothing else. Supported today: sdas (sdaslh5801 / sdasz80 .lst and the
// linker's relocated .rst) and zasm .lst.
//
// Neither assembler names the file of an included line in its listing, so
// the parsers walk the listing against the real source files (read through
// a SourceReader) and attribute each line by matching its text.

namespace debug {

struct ListingLine {
    uint16_t addr = 0;
    std::vector<uint8_t> bytes; ///< never empty: only lines that emit bytes are kept
    int file = 0;               ///< index into Listing::files
    int line = 0;               ///< 1-based line in that file
};

struct Listing {
    std::string format;               ///< "sdas", "zasm", "symbols"
    std::vector<std::string> files;   ///< absolute paths; [0] is the main source
    std::vector<ListingLine> lines;   ///< in listing order
    std::map<std::string, uint16_t> symbols; ///< labels and equates
    std::vector<std::string> warnings;       ///< non-fatal: unmatched lines, missing sources
};

/// Reads a text file as lines (without line terminators); false if it
/// can't be read. The default reads from disk; tests supply their own.
using SourceReader = std::function<bool(const std::string& path, std::vector<std::string>* lines)>;
SourceReader diskReader();

struct ListingInput {
    std::string path;                ///< the listing file (absolute or relative to cwd)
    std::vector<std::string> text;   ///< its lines
    std::string source;              ///< main source file if known (else derived by the format)
    SourceReader reader;
};

struct ListingFormat {
    const char* name;
    bool (*detect)(const std::vector<std::string>& text);
    bool (*parse)(const ListingInput& in, Listing* out, std::string* error);
};

/// The supported formats, tried in order by loadListing().
const std::vector<ListingFormat>& listingFormats();

/// Reads `path`, picks the format and parses it. `source` optionally names
/// the main source file (default: derived from the listing).
bool loadListing(const std::string& path, Listing* out, std::string* error,
                 const std::string& source = {}, SourceReader reader = diskReader());

/// A `.SYMBOLS:` table (lines of `HHHH name`) -- symbols only, no lines.
bool loadSymbolFile(const std::string& path, Listing* out, std::string* error, SourceReader reader = diskReader());

// Format parsers (listingFormats() entries), exposed for tests.
bool detectSdas(const std::vector<std::string>& text);
bool parseSdas(const ListingInput& in, Listing* out, std::string* error);
bool detectZasm(const std::vector<std::string>& text);
bool parseZasm(const ListingInput& in, Listing* out, std::string* error);

// ── Shared helpers for the parsers ────────────────────────────────────────

/// Absolute, normalised form of `path`; relative paths resolve against
/// `baseDir` (or the cwd when empty).
std::string absolutePath(const std::string& path, const std::string& baseDir = {});
std::string directoryOf(const std::string& path);

/// Attributes listing lines to source files by their text. Holds the
/// include stack; the parsers feed it one listing source text at a time.
class SourceWalker {
public:
    SourceWalker(Listing& listing, SourceReader reader) : m_listing(listing), m_reader(std::move(reader)) {}

    /// Opens the main file (index 0 of the listing's files).
    void begin(const std::string& mainFile);

    /// A listing line whose source is `text`, with the line number the
    /// assembler printed (sdas) or 0 when it prints none (zasm). Returns
    /// the (file, line) it belongs to, or false if it can't be placed
    /// (header/trailer text, or no source available). A line that is an
    /// include directive opens the included file next.
    bool place(const std::string& text, int printedLine, int* file, int* line);

    /// Number of listing lines that couldn't be matched to source text.
    int unmatched() const { return m_unmatched; }

private:
    struct Open {
        int file;
        std::vector<std::string> lines;
        int cursor = 0;   // next expected line index (0-based) -- for listings without line numbers
    };
    int fileIndex(const std::string& path);
    void push(const std::string& path);
    bool matches(const Open& f, int index, const std::string& text) const;
    void noteInclude(const std::string& text, const std::string& includerDir);

    Listing& m_listing;
    SourceReader m_reader;
    std::vector<Open> m_stack;
    int m_unmatched = 0;
};

/// Whitespace-collapsed, trimmed text for comparing listing and source.
std::string normalizeSourceText(const std::string& text);

/// The quoted file name of an include directive (`.include "x"`,
/// `#include "x"`, `include "x"`), or empty if `text` isn't one.
std::string includeTarget(const std::string& text);

} // namespace debug
