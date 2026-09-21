#include "PresetFile.hpp"

#include "PC1500Keyboard.hpp"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

struct RawLine {
    int indent;
    std::string content; // leading indent stripped
    int lineNo;
};

// Opens `path`, filling `error` with the errno detail on failure (e.g.
// "Permission denied" for a sandbox-denied path vs. "No such file or
// directory" for a genuinely missing one -- the two look identical from
// a bare open failure alone).
bool openFileOrError(const std::string& path, std::ios::openmode mode, std::ifstream* in,
                      const char* what, std::string* error) {
    errno = 0;
    in->open(path, mode);
    if (!*in) {
        *error = std::string("could not open ") + what + ": " + path + " (" + std::strerror(errno) + ")";
        return false;
    }
    return true;
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(' ');
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(' ');
    return s.substr(start, end - start + 1);
}

std::string unquote(const std::string& s) {
    // Handles both double- and single-quoted scalars -- YAML's two quoting
    // styles. Single quotes protect a colon-containing value from a real
    // YAML parser's flow-mapping ambiguity (e.g. a 'type' step like
    // '10 PRINT "Bank: 7"' would trip a real parser on the colon after
    // "Bank"). This parser only ever splits on a line's *first* colon (see
    // splitKeyValue), so it never needed the quoting to disambiguate, but
    // still strips the quotes here and passes the inside through verbatim
    // rather than choking on the leftover literal quote characters.
    // Deliberately doesn't implement YAML's '' -> ' escape (not needed by
    // any sample file this loader targets).
    if (s.size() >= 2 && s.front() == s.back() && (s.front() == '"' || s.front() == '\'')) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// Removes a trailing ` # ...` comment from a not-yet-unquoted scalar. A
// bare `#` is only a comment when whitespace-separated from the value (the
// YAML rule), so `PRINT A#B` / `A$#` keep their `#`. When the value is
// quoted, the comment can only sit *after* the closing quote, so anything
// past it (`'CALL X'   # note`) is dropped and `#` inside the quotes is
// left alone.
//
// NOT applied to a `- type:` step's payload -- that is keystroke-literal
// (a mid-line `#` is BASIC: `... AS #1`, `PRINT #1,...`). parseStepList
// re-reads the raw remainder for `type:` and skips this. Every other verb
// (`key:`, `wait:`, `trace:`, ...) still runs through here.
std::string stripInlineComment(const std::string& rest) {
    if (!rest.empty() && (rest.front() == '"' || rest.front() == '\'')) {
        size_t close = rest.find(rest.front(), 1);
        if (close != std::string::npos) return trim(rest.substr(0, close + 1));
        return rest; // unterminated quote -- leave it for unquote() to pass through
    }
    size_t hash = rest.find(" #");
    if (hash != std::string::npos) return trim(rest.substr(0, hash));
    return rest;
}

// Splits "key" or "key: value" (the value already trimmed/unquoted).
// hasInline is false for a bare "key:" (a block-only field).
bool splitKeyValue(const std::string& content, std::string* key, std::string* value, bool* hasInline) {
    size_t colon = content.find(':');
    if (colon == std::string::npos) return false;
    *key = trim(content.substr(0, colon));
    std::string rest = stripInlineComment(trim(content.substr(colon + 1)));
    *hasInline = !rest.empty();
    *value = unquote(rest);
    return !key->empty();
}

bool readLines(const std::string& path, std::vector<RawLine>* out, std::string* error) {
    std::ifstream in;
    if (!openFileOrError(path, std::ios::in, &in, "file", error)) return false;
    std::string raw;
    int lineNo = 0;
    while (std::getline(in, raw)) {
        lineNo++;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        if (raw.find('\t') != std::string::npos) {
            *error = "line " + std::to_string(lineNo) + ": tabs are not supported, use 2-space indentation";
            return false;
        }
        size_t firstNonSpace = raw.find_first_not_of(' ');
        if (firstNonSpace == std::string::npos) continue; // blank line
        if (raw[firstNonSpace] == '#') continue; // full-line comment
        out->push_back(RawLine{static_cast<int>(firstNonSpace), raw.substr(firstNonSpace), lineNo});
    }
    return true;
}

std::string resolvePath(const std::filesystem::path& dir, const std::string& value) {
    std::filesystem::path p(value);
    if (p.is_absolute()) return p.string();
    return (dir / p).lexically_normal().string();
}

bool parseStepList(const std::vector<RawLine>& lines, size_t& idx, std::vector<PresetStep>* out,
                   std::string* error) {
    if (idx >= lines.size() || lines[idx].indent == 0) {
        *error = "line " + std::to_string(idx < lines.size() ? lines[idx].lineNo : lines.back().lineNo) +
                 ": expected an indented list";
        return false;
    }
    int itemIndent = lines[idx].indent;
    while (idx < lines.size() && lines[idx].indent == itemIndent) {
        const RawLine& line = lines[idx];
        if (line.content.size() < 2 || line.content[0] != '-' || line.content[1] != ' ') {
            *error = "line " + std::to_string(line.lineNo) + ": expected '- verb: value' list item";
            return false;
        }
        std::string verb, value;
        bool hasInline;
        if (!splitKeyValue(line.content.substr(2), &verb, &value, &hasInline)) {
            *error = "line " + std::to_string(line.lineNo) + ": malformed step (expected 'verb: value')";
            return false;
        }
        // `- wait:` / `- syncclock:` alone (no value) are valid -- see their
        // branches below.
        if (!hasInline && verb != "wait" && verb != "syncclock") {
            *error = "line " + std::to_string(line.lineNo) + ": malformed step (expected 'verb: value')";
            return false;
        }
        PresetStep step;
        if (verb == "key") {
            // A `key:` step presses one physical key by name. A multi-char
            // value that isn't a known key name (e.g. `key: X=34`,
            // `key: MEM`, `key: CALL &4100,X`) would otherwise press nothing
            // and the loader would carry on silently -- catch it here and
            // point at `type:`, which is what such a value wants.
            if (value != "break" && value != "on" &&
                PC1500Keyboard::keyFromName(value) == PC1500Keyboard::Key::Unknown) {
                *error = "line " + std::to_string(line.lineNo) + ": '" + value +
                         "' is not a key name -- use 'type: " + value +
                         "' to send it as keystrokes, or 'key:' with a single key";
                return false;
            }
            step.kind = PresetStep::Kind::Key;
            step.text = value;
        } else if (verb == "type") {
            step.kind = PresetStep::Kind::Type;
            // A `type:` payload is sent verbatim as keystrokes, so -- unlike
            // every other verb -- it is NOT run through stripInlineComment:
            // a mid-line `#` is BASIC (`OPEN ... AS #1`, `PRINT #1,...`,
            // `PRINT#1,"...^C..."`), never a trailing `# comment`. Recover
            // the raw remainder of the line here (splitKeyValue already
            // dropped a real comment from `value`). Surrounding matched
            // quotes are still stripped, since a colon-bearing `type` value
            // is conventionally wrapped in quotes (see unquote()). The
            // trade-off: a `# note` written after a `type:` step is now
            // typed literally too -- put comments on their own line.
            const std::string afterDash = line.content.substr(2);   // "type: <payload>"
            step.text = unquote(trim(afterDash.substr(afterDash.find(':') + 1)));
        } else if (verb == "wait") {
            step.kind = PresetStep::Kind::Wait;
            if (!hasInline) {
                // `- wait:` with no value -- block until the ROM's keyboard
                // idle loop re-engages (a long program / plot has finished).
                step.waitSeconds = PresetStep::kWaitUntilIdle;
            } else {
                try {
                    step.waitSeconds = std::stod(value);
                } catch (...) {
                    *error = "line " + std::to_string(line.lineNo) + ": invalid 'wait' value '" + value + "'";
                    return false;
                }
                if (step.waitSeconds < 0) {
                    *error = "line " + std::to_string(line.lineNo) +
                             ": 'wait' value must not be negative (use `- wait:` with no value to "
                             "wait for the program to finish)";
                    return false;
                }
            }
        } else if (verb == "trace") {
            // Port of Calc-U-59's `KEYSTROKES:` `Trace:` directive.
            // `- trace: off` (case-insensitive) stops the current capture;
            // any other value is the output filename. The filename must
            // not contain a path separator -- WHERE it lands is the trace
            // directory's concern (see PC1500PresetLoader::applyPC1500Preset()).
            std::string lowered = value;
            for (char& ch : lowered) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            step.kind = PresetStep::Kind::Trace;
            if (lowered == "off") {
                step.text = "";
            } else if (value.find('/') != std::string::npos || value.find('\\') != std::string::npos) {
                *error = "line " + std::to_string(line.lineNo) + ": 'trace' filename '" + value +
                         "' must not contain a path separator";
                return false;
            } else {
                step.text = value;
            }
        } else if (verb == "screenshot") {
            // PNG of the LCD dot matrix into the trace directory -- like
            // `trace:`, WHERE it lands is the loader's concern, so the
            // filename must not carry a path.
            step.kind = PresetStep::Kind::Screenshot;
            if (value.empty()) {
                *error = "line " + std::to_string(line.lineNo) + ": 'screenshot' needs a filename";
                return false;
            }
            if (value.find('/') != std::string::npos || value.find('\\') != std::string::npos) {
                *error = "line " + std::to_string(line.lineNo) + ": 'screenshot' filename '" + value +
                         "' must not contain a path separator";
                return false;
            }
            step.text = value;
        } else if (verb == "syncclock") {
            // Re-seed the RTC from the host clock (see the loaders). Takes
            // no value.
            if (hasInline && !value.empty()) {
                *error = "line " + std::to_string(line.lineNo) + ": 'syncclock' takes no value (write `- syncclock:`)";
                return false;
            }
            step.kind = PresetStep::Kind::SyncClock;
        } else if (verb == "check") {
            *error = "line " + std::to_string(line.lineNo) + ": 'check' steps are not yet supported by this loader";
            return false;
        } else {
            *error = "line " + std::to_string(line.lineNo) + ": unrecognized step verb '" + verb + "'";
            return false;
        }
        out->push_back(step);
        idx++;
    }
    return true;
}

// A `memory-expansion*:` block -- a one-item list, nothing else (no
// address:/banks:/etc.; a second item or an extra field is a hard parse
// error, matching this parser's all-or-nothing philosophy). The item is
// one of:
//   * `- modulespecfile: <path>` -- a software-defined-module definition
//                                   FILE, resolved relative to `presetDir`
//                                   and written to `*specFileTarget`.
//   * `- modulespec: <module-name>` -- a bundled/standard module named by
//                                   its `module-name:`, stored verbatim in
//                                   `*specNameTarget` for the loader to
//                                   resolve against its module directory.
// `blockLabel` names the block in error text.
bool parseModuleListBlock(const std::vector<RawLine>& lines, size_t& idx, const char* blockLabel,
                          const std::filesystem::path& presetDir, std::string* specFileTarget,
                          std::string* specNameTarget, std::string* error) {
    if (idx >= lines.size() || lines[idx].indent == 0) {
        *error = "line " + std::to_string(idx < lines.size() ? lines[idx].lineNo : lines.back().lineNo) +
                 ": '" + blockLabel + ":' requires an indented list";
        return false;
    }
    int itemIndent = lines[idx].indent;
    int itemCount = 0;
    while (idx < lines.size() && lines[idx].indent == itemIndent) {
        const RawLine& line = lines[idx];
        if (line.content.size() < 2 || line.content[0] != '-' || line.content[1] != ' ') {
            *error = "line " + std::to_string(line.lineNo) +
                     ": expected '- modulespec: <module-name>' or '- modulespecfile: <path>' list item";
            return false;
        }
        itemCount++;
        if (itemCount > 1) {
            *error = "line " + std::to_string(line.lineNo) + ": only one '" + blockLabel +
                     "' item is supported by this loader";
            return false;
        }
        std::string key, value;
        bool hasInline;
        if (!splitKeyValue(line.content.substr(2), &key, &value, &hasInline) || !hasInline ||
            (key != "modulespec" && key != "modulespecfile")) {
            *error = "line " + std::to_string(line.lineNo) +
                     ": expected 'modulespec: <module-name>' or 'modulespecfile: <path>'" +
                     (key == "module" ? " (the built-in 'module:' names are gone -- use e.g. "
                                        "'modulespec: CE-155')"
                                      : "");
            return false;
        }
        if (key == "modulespecfile") {
            *specFileTarget = resolvePath(presetDir, value);
        } else {
            *specNameTarget = value;
        }
        idx++;
        if (idx < lines.size() && lines[idx].indent > itemIndent) {
            *error = "line " + std::to_string(lines[idx].lineNo) + ": '" + value + "' takes no additional fields";
            return false;
        }
    }
    if (itemCount == 0) {
        *error = std::string("'") + blockLabel + ":' requires at least one item";
        return false;
    }
    return true;
}

bool parseProgramBlock(const std::vector<RawLine>& lines, size_t& idx, const std::filesystem::path& presetDir,
                        PresetProgram* out, std::string* error) {
    if (idx >= lines.size() || lines[idx].indent == 0) {
        *error = "'program:' requires an indented block";
        return false;
    }
    int blockIndent = lines[idx].indent;
    PresetProgram prog;
    std::string formatStr = "binary";
    bool hasPath = false, hasAddress = false, hasText = false, hasSlot = false, hasLength = false;

    while (idx < lines.size() && lines[idx].indent == blockIndent) {
        const RawLine& line = lines[idx];
        std::string key, value;
        bool hasInline;
        if (!splitKeyValue(line.content, &key, &value, &hasInline)) {
            *error = "line " + std::to_string(line.lineNo) + ": malformed 'program' field";
            return false;
        }
        idx++;
        if (key == "format") {
            if (!hasInline) { *error = "line " + std::to_string(line.lineNo) + ": 'format' requires a value"; return false; }
            formatStr = value;
        } else if (key == "path") {
            if (!hasInline) { *error = "line " + std::to_string(line.lineNo) + ": 'path' requires a value"; return false; }
            prog.path = resolvePath(presetDir, value);
            hasPath = true;
        } else if (key == "address") {
            if (!hasInline) { *error = "line " + std::to_string(line.lineNo) + ": 'address' requires a value"; return false; }
            try {
                prog.address = static_cast<uint16_t>(std::stoul(value, nullptr, 16));
            } catch (...) {
                *error = "line " + std::to_string(line.lineNo) + ": invalid hex 'address' value '" + value + "'";
                return false;
            }
            hasAddress = true;
            prog.hasAddress = true;
        } else if (key == "slot") {
            if (!hasInline) { *error = "line " + std::to_string(line.lineNo) + ": 'slot' requires a value"; return false; }
            std::string v = value;
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "s0") prog.slot = PresetProgram::Slot::S0;
            else if (v == "s1") prog.slot = PresetProgram::Slot::S1;
            else if (v == "s2") prog.slot = PresetProgram::Slot::S2;
            else {
                *error = "line " + std::to_string(line.lineNo) + ": 'slot' must be S0, S1, or S2";
                return false;
            }
            hasSlot = true;
        } else if (key == "length") {
            if (!hasInline) { *error = "line " + std::to_string(line.lineNo) + ": 'length' requires a value"; return false; }
            bool bad = value.empty();
            unsigned long v = 0;
            if (!bad) {
                try {
                    size_t pos = 0;
                    int base = (value.size() > 2 && value[0] == '0' &&
                                (value[1] == 'x' || value[1] == 'X'))
                                   ? 16
                                   : 10;
                    v = std::stoul(value, &pos, base);
                    bad = (pos != value.size());
                } catch (...) {
                    bad = true;
                }
            }
            if (bad) {
                *error = "line " + std::to_string(line.lineNo) + ": invalid 'length' value '" + value + "'";
                return false;
            }
            prog.length = static_cast<uint32_t>(v);
            prog.hasLength = true;
            hasLength = true;
        } else if (key == "text") {
            if (!hasInline || value != "|") {
                *error = "line " + std::to_string(line.lineNo) + ": 'text' must be a block scalar ('text: |')";
                return false;
            }
            if (idx >= lines.size() || lines[idx].indent <= blockIndent) {
                *error = "line " + std::to_string(line.lineNo) + ": 'text: |' requires indented content";
                return false;
            }
            int textIndent = lines[idx].indent;
            std::string collected;
            while (idx < lines.size() && lines[idx].indent >= textIndent) {
                collected += std::string(static_cast<size_t>(lines[idx].indent - textIndent), ' ') + lines[idx].content;
                collected += "\n";
                idx++;
            }
            prog.text = collected;
            hasText = true;
        } else {
            *error = "line " + std::to_string(line.lineNo) + ": unrecognized 'program' field '" + key + "'";
            return false;
        }
    }

    // `slot:` / `length:` belong to a machine-language block only -- reject
    // them for the BASIC formats up front, before any per-format work
    // (e.g. opening the `path:` file) can mask the message.
    if (formatStr != "binary" && (hasSlot || hasLength)) {
        *error = std::string("'") + (hasSlot ? "slot" : "length") +
                 "' is only valid with 'format: binary'";
        return false;
    }

    if (formatStr == "binary") {
        prog.format = PresetProgram::Format::Binary;
        if (!hasPath) { *error = "'program: format: binary' requires 'path'"; return false; }
        if (hasText) { *error = "'text' is only valid with 'format: basic-text'"; return false; }
        if (hasSlot) {
            // PC-1600 machine-language: bytes go linearly into the one named
            // slot. `address` / `length` are optional -- a 16-byte PC-1600
            // ML header supplies them -- and each overrides its header field
            // when given. (The loader requires both when the file has no
            // header.)
        } else {
            // PC-1500-style: the whole file poked at a fixed address.
            if (!hasAddress) {
                *error = "'program: format: binary' requires 'address' (or 'slot: S0|S1|S2' for "
                         "a PC-1600 preset)";
                return false;
            }
            if (hasLength) {
                *error = "'length' is only valid with 'slot:' (PC-1600 machine-language loading)";
                return false;
            }
        }
    } else if (formatStr == "basic-text") {
        prog.format = PresetProgram::Format::BasicText;
        if (!hasText && !hasPath) {
            *error = "'program: format: basic-text' requires 'text' or 'path'";
            return false;
        }
        if (!hasText && hasPath) {
            std::ifstream in;
            if (!openFileOrError(prog.path, std::ios::in, &in, "program file", error)) return false;
            std::ostringstream buf;
            buf << in.rdbuf();
            prog.text = buf.str();
        }
    } else if (formatStr == "basic-binary" || formatStr == "basic-tokenized") {
        prog.format = PresetProgram::Format::BasicBinary;
        if (!hasPath) {
            *error = "'program: format: basic-binary' requires 'path' (a plain-text BASIC "
                     "listing, tokenized on load)";
            return false;
        }
        if (hasText) { *error = "'text' is only valid with 'format: basic-text'"; return false; }
        if (hasAddress) {
            *error = "'address' is not valid with 'format: basic-binary' (the load address "
                     "comes from the machine's BASIC pointers)";
            return false;
        }
        std::ifstream in;
        if (!openFileOrError(prog.path, std::ios::binary, &in, "program file", error)) return false;
    } else {
        *error = "unrecognized program format '" + formatStr + "'";
        return false;
    }

    *out = prog;
    return true;
}

} // namespace

bool parsePresetFile(const std::string& path, PresetFile* out, std::string* error) {
    std::vector<RawLine> lines;
    if (!readLines(path, &lines, error)) return false;

    std::filesystem::path presetDir = std::filesystem::path(path).parent_path();
    if (presetDir.empty()) presetDir = ".";

    std::string modelRom;  // the ROM suffix of `model: NAME:ROM`, verbatim ("" = none given)
    std::string plotter;  // normalized `plotter:` name ("" / "ce1600p" / "ce150"), suffix stripped
    std::string plotterRom;  // the ROM suffix of `plotter: NAME:ROM`, lower-cased ("" = none given)
    bool hasPlotter = false;
    std::string floppy;  // `floppy:` value with any `,A`/`,B` suffix stripped
    int floppySide = 0;  // 0 = A, 1 = B, parsed from that suffix
    bool hasFloppy = false;

    size_t idx = 0;
    while (idx < lines.size()) {
        const RawLine& line = lines[idx];
        if (line.indent != 0) {
            *error = "line " + std::to_string(line.lineNo) + ": unexpected indentation";
            return false;
        }
        std::string key, value;
        bool hasInline;
        if (!splitKeyValue(line.content, &key, &value, &hasInline)) {
            *error = "line " + std::to_string(line.lineNo) + ": malformed field";
            return false;
        }
        idx++;
        if (key == "model") {
            if (!hasInline) { *error = "'model' requires a value"; return false; }
            // `model: NAME[:ROM]` -- the ROM revision rides on the model
            // (`PC-1500:A01`, `PC-1600:new`). `model` stays the bare name so
            // everything keyed off it is unaffected; the suffix is
            // validated per model below.
            const size_t colon = value.find(':');
            out->model = value.substr(0, colon);
            if (colon != std::string::npos) {
                modelRom = value.substr(colon + 1);
                if (modelRom.empty()) {
                    *error = "line " + std::to_string(line.lineNo) + ": 'model: " + value +
                             "' has an empty ROM after ':'";
                    return false;
                }
            }
        } else if (key == "firmware") {
            *error = "line " + std::to_string(line.lineNo) +
                     ": 'firmware:' is no longer supported -- put the ROM on the model instead "
                     "(e.g. 'model: PC-1500:A01' or 'model: PC-1600:old')";
            return false;
        } else if (key == "program") {
            if (hasInline) { *error = "line " + std::to_string(line.lineNo) + ": 'program:' takes a block, not an inline value"; return false; }
            PresetSection section;
            section.kind = PresetSection::Kind::Program;
            if (!parseProgramBlock(lines, idx, presetDir, &section.program, error)) return false;
            out->sections.push_back(std::move(section));
        } else if (key == "keys") {
            if (hasInline) { *error = "line " + std::to_string(line.lineNo) + ": 'keys:' takes a list, not an inline value"; return false; }
            PresetSection section;
            section.kind = PresetSection::Kind::Keys;
            if (!parseStepList(lines, idx, &section.keys, error)) return false;
            out->sections.push_back(std::move(section));
        } else if (key == "pre-load-keys" || key == "post-load-keys") {
            *error = "line " + std::to_string(line.lineNo) + ": '" + key +
                     "' is no longer supported -- use a 'keys:' block instead (repeatable, executed in file order)";
            return false;
        } else if (key == "memory-expansion") {
            if (hasInline) { *error = "line " + std::to_string(line.lineNo) + ": 'memory-expansion:' takes a list, not an inline value"; return false; }
            if (!parseModuleListBlock(lines, idx, "memory-expansion", presetDir,
                                      &out->memoryExpansionModuleSpecFile,
                                      &out->memoryExpansionModuleSpecName, error))
                return false;
        } else if (key == "memory-expansion-1" || key == "memory-expansion-2") {
            if (hasInline) { *error = "line " + std::to_string(line.lineNo) + ": '" + key + ":' takes a list, not an inline value"; return false; }
            const bool slot1 = (key == "memory-expansion-1");
            std::string* specFileTarget = slot1 ? &out->slot1ModuleSpecFile : &out->slot2ModuleSpecFile;
            std::string* specNameTarget = slot1 ? &out->slot1ModuleSpecName : &out->slot2ModuleSpecName;
            if (!parseModuleListBlock(lines, idx, key.c_str(), presetDir, specFileTarget,
                                      specNameTarget, error))
                return false;
        } else if (key == "plotter") {
            if (!hasInline) { *error = "'plotter' requires a value"; return false; }
            hasPlotter = true;
            plotter = value;
            for (char& ch : plotter) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            // `plotter: NAME[:ROM]` -- only the CE-1600P has a ROM choice
            // (`plotter: ce1600p:old`); split it off before normalizing.
            const size_t plotterColon = plotter.find(':');
            if (plotterColon != std::string::npos) {
                plotterRom = plotter.substr(plotterColon + 1);
                plotter.resize(plotterColon);
                if (plotterRom.empty()) {
                    *error = "line " + std::to_string(line.lineNo) + ": 'plotter: " + value +
                             "' has an empty ROM after ':'";
                    return false;
                }
            }
            // Accept a couple of spellings; normalize to the canonical token.
            if (plotter == "ce-1600p") plotter = "ce1600p";
            else if (plotter == "ce-150") plotter = "ce150";
            else if (plotter == "none" || plotter == "off") plotter.clear();
            if (!plotter.empty() && plotter != "ce1600p" && plotter != "ce150") {
                *error = "line " + std::to_string(line.lineNo) +
                         ": 'plotter: " + value + "' is not a known plotter (expected ce1600p or ce150)";
                return false;
            }
            if (!plotterRom.empty() && plotter != "ce1600p") {
                *error = "line " + std::to_string(line.lineNo) + ": 'plotter: " + value +
                         "' -- only the CE-1600P has a ROM choice ('plotter: ce1600p:new|old')";
                return false;
            }
            if (!plotterRom.empty() && plotterRom != "new" && plotterRom != "old") {
                *error = "line " + std::to_string(line.lineNo) + ": 'plotter: " + value +
                         "' -- the CE-1600P ROM must be 'new' or 'old'";
                return false;
            }
        } else if (key == "floppy") {
            if (!hasInline) { *error = "'floppy' requires a value"; return false; }
            hasFloppy = true;
            floppy = value;
            // Strip an optional trailing `,A`/`,B` (case-insensitive) side
            // selector -- e.g. `floppy: mydisk,B`.
            const size_t comma = floppy.rfind(',');
            if (comma != std::string::npos) {
                std::string suffix = floppy.substr(comma + 1);
                for (char& ch : suffix) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                if (suffix == "A" || suffix == "B") {
                    floppySide = (suffix == "B") ? 1 : 0;
                    floppy = floppy.substr(0, comma);
                } else {
                    *error = "line " + std::to_string(line.lineNo) +
                             ": 'floppy: " + value + "' has an invalid side suffix (expected ,A or ,B)";
                    return false;
                }
            }
        } else if (key == "rom-modules") {
            *error = "'" + key + "' is not yet supported by this loader";
            return false;
        } else {
            *error = "line " + std::to_string(line.lineNo) + ": unrecognized field '" + key + "'";
            return false;
        }
    }

    if (out->model.empty()) {
        *error = "'model' is required";
        return false;
    }
    if (out->model == "PC-1600") {
        // A PC-1600 preset is model + memory slots + keys only. Reject the
        // PC-1500-only pieces with a clear message rather than silently
        // ignoring them.
        // `model: PC-1600:new|old` picks the calculator ROM version (default
        // "new"); the CE-1600P ROM (`plotter: ce1600p:new|old`) is
        // independent of it.
        out->romVariant = "new";
        if (!modelRom.empty()) {
            for (char& ch : modelRom) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (modelRom != "new" && modelRom != "old") {
                *error = "'model: PC-1600:" + modelRom + "' -- the PC-1600 ROM must be 'new' or 'old'";
                return false;
            }
            out->romVariant = modelRom;
        }
        out->ce1600pRomVariant = plotterRom.empty() ? "new" : plotterRom;
        if (!out->memoryExpansionModuleSpecFile.empty() || !out->memoryExpansionModuleSpecName.empty()) {
            *error = "use 'memory-expansion-1:' / 'memory-expansion-2:' for a PC-1600 preset, not 'memory-expansion:'";
            return false;
        }
        for (const PresetSection& s : out->sections) {
            // `format: basic-text` (typed through the line editor),
            // `format: basic-binary` (a tokenized transfer file / a `.bas`
            // listing tokenized on load) and `format: binary` (a machine-
            // language block loaded linearly into one slot -- see
            // PC1600PresetLoader.cpp) are all supported. A `binary` block
            // must name its target slot.
            if (s.kind == PresetSection::Kind::Program &&
                s.program.format == PresetProgram::Format::Binary &&
                s.program.slot == PresetProgram::Slot::None) {
                *error = "'program: format: binary' requires 'slot: S0|S1|S2' for a PC-1600 preset "
                         "(S0 = internal RAM, S1/S2 = the memory slots)";
                return false;
            }
        }
        out->plotter = plotter;  // already validated/normalized above
        if (hasFloppy && plotter != "ce1600p") {
            *error = "'floppy:' requires 'plotter: ce1600p' (the CE-1600F attaches as a union with it)";
            return false;
        }
        out->floppy = floppy;
        out->floppySide = floppySide;
        return true;
    }
    // The remaining branches are PC-1500/1500A -- the per-slot blocks are
    // PC-1600 only.
    if (!out->slot1ModuleSpecFile.empty() || !out->slot1ModuleSpecName.empty() ||
        !out->slot2ModuleSpecFile.empty() || !out->slot2ModuleSpecName.empty()) {
        *error = "'memory-expansion-1:' / 'memory-expansion-2:' are only valid for a PC-1600 preset";
        return false;
    }
    for (const PresetSection& s : out->sections) {
        if (s.kind == PresetSection::Kind::Program &&
            s.program.slot != PresetProgram::Slot::None) {
            *error = "'program: slot:' is only valid for a PC-1600 preset (a PC-1500 'format: "
                     "binary' block pokes the whole file at 'address:')";
            return false;
        }
    }
    if (hasFloppy) {
        *error = "'floppy:' is only valid for a PC-1600 preset (the CE-1600F is a PC-1600 device)";
        return false;
    }
    if (hasPlotter) {
        // The PC-1500 family takes the CE-150 (via the 60-pin bus); the
        // CE-1600P is a PC-1600 device.
        if (plotter == "ce1600p") {
            *error = "'plotter: ce1600p' is a PC-1600 device -- use 'plotter: ce150' on a "
                     "PC-1500 preset";
            return false;
        }
        // `plotter: none` normalizes to empty above; anything left that
        // isn't "ce150" was already rejected by the parser.
        out->plotter = plotter; // "" or "ce150"
    }
    // `model: PC-1500:A01|A03|A04` / `model: PC-1500A:A04` -- the ROM
    // revision rides on the model. WHERE the file lives is deliberately not
    // this struct's concern (see PresetFile::romVariant's doc comment).
    for (char& ch : modelRom) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (out->model == "PC-1500A") {
        out->variant = PC1500Variant::PC1500A;
        // The PC-1500A can only run A04 (the default).
        if (!modelRom.empty() && modelRom != "A04") {
            *error = "'model: PC-1500A:" + modelRom + "' -- the PC-1500A can only run ROM A04";
            return false;
        }
        out->romVariant = "A04";
    } else if (out->model == "PC-1500") {
        out->variant = PC1500Variant::PC1500;
        out->romVariant = "A04";
        if (!modelRom.empty()) {
            if (modelRom != "A01" && modelRom != "A03" && modelRom != "A04") {
                *error = "'model: PC-1500:" + modelRom + "' -- the PC-1500 ROM must be A01, A03 or A04";
                return false;
            }
            out->romVariant = modelRom;
        }
    } else {
        *error = "unsupported model '" + out->model + "' (must be 'PC-1500', 'PC-1500A' or 'PC-1600')";
        return false;
    }

    return true;
}
