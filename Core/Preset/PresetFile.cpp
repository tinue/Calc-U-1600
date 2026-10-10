#include "PresetFile.hpp"
#include "PresetDebugBlock.hpp"

#include "../PC1500/PC1500Keyboard.hpp"
#include "../MachineCodeFile.hpp"
#include "../ProgramFile.hpp"
#include "../Connector/FloppyImageFile.hpp"
#include "../Connector/MemoryCardCatalog.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

std::string resolvePath(const std::filesystem::path& dir, const std::string& value) {
    std::string v = value;
    if (v == "~" || v.rfind("~/", 0) == 0) {
        if (const char* home = std::getenv("HOME")) v = std::string(home) + v.substr(1);
    }
    std::filesystem::path p(v);
    if (p.is_absolute()) return p.lexically_normal().string();
    return (dir / p).lexically_normal().string();
}

bool parseNumber(const std::string& value, uint32_t* out) {
    std::string digits = value;
    int base = 10;
    if (digits.rfind("0x", 0) == 0 || digits.rfind("0X", 0) == 0) {
        digits = digits.substr(2);
        base = 16;
    } else if (!digits.empty() && (digits[0] == '&' || digits[0] == '$')) {
        digits = digits.substr(1);
        base = 16;
    }
    if (digits.empty()) return false;
    for (char ch : digits)
        if (base == 16 ? !std::isxdigit(static_cast<unsigned char>(ch)) : !std::isdigit(static_cast<unsigned char>(ch)))
            return false;
    errno = 0;
    const unsigned long long v = std::strtoull(digits.c_str(), nullptr, base);
    if (errno != 0 || v > 0xFFFFFFFFull) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

namespace {

// ASCII-lowercases `s` in place (preset keywords/values are case-insensitive).
void lowerAscii(std::string& s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

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
    // styles, for a name or path with a leading/trailing space or a ` #`.
    // Not applied to a `type:` payload, which is typed as written. No
    // YAML '' -> ' escape.
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
// re-reads the raw remainder for `type:` and skips this and unquote().
// Every other verb (`key:`, `wait:`, `trace:`, ...) still runs through here.
std::string stripInlineComment(const std::string& rest) {
    // Nothing but a comment (`keys:   # after the boot`): the value is
    // empty, so a block key keeps working with a note after it.
    if (!rest.empty() && rest.front() == '#') return "";
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

// A Sharp product name as Sharp writes it (`CE-1600P`), in any case.
bool isProductName(const std::string& value, const char* name) {
    std::string a = value, b = name;
    lowerAscii(a);
    lowerAscii(b);
    return a == b;
}

bool parseStepList(const std::vector<RawLine>& lines, size_t& idx, const std::filesystem::path& presetDir,
                   std::vector<PresetStep>* out, std::string* error) {
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
            // A `type:` payload is typed exactly as written, to the end of
            // the line: not run through stripInlineComment (a mid-line `#`
            // is BASIC: `OPEN ... AS #1`, `PRINT #1,...`) nor unquote()
            // (`"SAVE LOAD"` types its quotes). A `# note` after a `type:`
            // step is typed too -- put comments on their own line.
            const std::string afterDash = line.content.substr(2);   // "type: <payload>"
            step.text = trim(afterDash.substr(afterDash.find(':') + 1));
        } else if (verb == "wait") {
            step.kind = PresetStep::Kind::Wait;
            if (!hasInline) {
                // `- wait:` with no value -- block until the ROM's keyboard
                // idle loop re-engages (a long program / plot has finished).
                step.waitSeconds = PresetStep::kWaitUntilIdle;
            } else {
                size_t used = 0;
                try {
                    step.waitSeconds = std::stod(value, &used);
                } catch (...) {
                    used = 0;
                }
                if (used == 0 || used != value.size()) {
                    *error = "line " + std::to_string(line.lineNo) + ": invalid 'wait' value '" + value +
                             "' (seconds, e.g. 'wait: 2' or 'wait: 0.5')";
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
            // `- trace: off` (case-insensitive) stops the current capture;
            // any other value is the output filename. The filename must
            // not contain a path separator -- WHERE it lands is the trace
            // directory's concern (see PC1500PresetLoader::applyPC1500Preset()).
            std::string lowered = value;
            lowerAscii(lowered);
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
        } else if (verb == "expect") {
            // Verbatim to the end of the line, like `type:` -- the expected
            // text may well contain `#` or quotes.
            step.kind = PresetStep::Kind::Expect;
            const std::string afterDash = line.content.substr(2);
            step.text = trim(afterDash.substr(afterDash.find(':') + 1));
            if (step.text.empty()) {
                *error = "line " + std::to_string(line.lineNo) + ": 'expect' needs the text to look for";
                return false;
            }
        } else if (verb == "syncclock") {
            // Re-seed the RTC from the host clock (see the loaders). Takes
            // no value.
            if (hasInline && !value.empty()) {
                *error = "line " + std::to_string(line.lineNo) + ": 'syncclock' takes no value (write `- syncclock:`)";
                return false;
            }
            step.kind = PresetStep::Kind::SyncClock;
        } else if (verb == "saveas") {
            // `- saveas: template|live <target>:<name>` or `... <target>:file:<path>`
            // (see PresetFile.hpp's top-of-file doc comment). The leading
            // word is required; the rest splits on ITS first colon
            // (mirroring splitKeyValue's own rule) into target/name.
            const std::string usage = "line " + std::to_string(line.lineNo) +
                                      ": expected 'saveas: template|live <slot-1|slot-2|floppy>:<name>' or "
                                      "'saveas: template|live <slot-1|slot-2|floppy>:file:<path>'";
            const size_t space = value.find_first_of(" \t");
            std::string kind = space == std::string::npos ? value : value.substr(0, space);
            lowerAscii(kind);
            if (kind != "template" && kind != "live") {
                *error = usage + " -- the first word must be 'template' or 'live'";
                return false;
            }
            step.saveAsTemplate = kind == "template";
            const std::string rest = space == std::string::npos ? "" : trim(value.substr(space + 1));
            const size_t targetColon = rest.find(':');
            if (targetColon == std::string::npos) {
                *error = usage;
                return false;
            }
            std::string target = trim(rest.substr(0, targetColon));
            std::string name = trim(rest.substr(targetColon + 1));
            lowerAscii(target);
            if (target == "slot-1") step.saveAsTarget = PresetStep::SaveAsTarget::Slot1;
            else if (target == "slot-2") step.saveAsTarget = PresetStep::SaveAsTarget::Slot2;
            else if (target == "floppy") step.saveAsTarget = PresetStep::SaveAsTarget::Floppy;
            else {
                *error = "line " + std::to_string(line.lineNo) + ": 'saveas: " + value +
                         "' -- target must be 'slot-1', 'slot-2' or 'floppy'";
                return false;
            }
            if (name.rfind("file:", 0) == 0) {
                // The file form: the name is the file name minus the suffix
                // the target's catalog uses.
                const std::string path = trim(name.substr(5));
                const std::string suffix =
                    step.saveAsTarget == PresetStep::SaveAsTarget::Floppy ? kFloppyFileSuffix : kCardFileSuffix;
                const std::string fileName = std::filesystem::path(path).filename().string();
                if (!named_file_detail::hasSuffix(fileName, suffix)) {
                    *error = "line " + std::to_string(line.lineNo) + ": 'saveas: " + value + "' -- the file must end in '" +
                             suffix + "'";
                    return false;
                }
                step.saveAsPath = resolvePath(presetDir, path);
                name = fileName.substr(0, fileName.size() - suffix.size());
            }
            if (name.empty()) {
                *error = "line " + std::to_string(line.lineNo) + ": 'saveas:' needs a name";
                return false;
            }
            if (name.find('"') != std::string::npos) {
                *error = "line " + std::to_string(line.lineNo) + ": 'saveas:' name must not contain '\"'";
                return false;
            }
            step.kind = PresetStep::Kind::SaveAs;
            step.text = name;
        } else {
            *error = "line " + std::to_string(line.lineNo) + ": unrecognized step verb '" + verb + "'";
            return false;
        }
        out->push_back(step);
        idx++;
    }
    return true;
}

bool readProgramBytes(const std::string& path, std::vector<uint8_t>* out, std::string* error) {
    std::ifstream in;
    if (!openFileOrError(path, std::ios::binary, &in, "program file", error)) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// `program:` -- one of
//   file: <path>          loaded by what the file holds (Core/ProgramFile):
//                         BASIC (a listing or tokenized) goes straight into
//                         memory, machine code (with or without a header)
//                         through the Load Machine Code rules;
//   file: + typed: true   a BASIC listing, typed in line by line;
//   text: |               a BASIC listing written inline, typed in.
// `address:` / `length:` go with machine code only.
bool parseProgramBlock(const std::vector<RawLine>& lines, size_t& idx, const std::filesystem::path& presetDir,
                        PresetProgram* out, std::string* error) {
    if (idx >= lines.size() || lines[idx].indent == 0) {
        *error = "'program:' requires an indented block";
        return false;
    }
    int blockIndent = lines[idx].indent;
    PresetProgram prog;
    bool hasFile = false, hasAddress = false, hasText = false, hasLength = false, typed = false;
    int blockLine = lines[idx].lineNo;

    while (idx < lines.size() && lines[idx].indent == blockIndent) {
        const RawLine& line = lines[idx];
        std::string key, value;
        bool hasInline;
        if (!splitKeyValue(line.content, &key, &value, &hasInline)) {
            *error = "line " + std::to_string(line.lineNo) + ": malformed 'program' field";
            return false;
        }
        idx++;
        const std::string at = "line " + std::to_string(line.lineNo) + ": ";
        if (key != "text" && !hasInline) {
            *error = at + "'" + key + "' requires a value";
            return false;
        }
        if (key == "file") {
            prog.path = resolvePath(presetDir, value);
            hasFile = true;
        } else if (key == "typed") {
            std::string v = value;
            lowerAscii(v);
            if (v != "true" && v != "false") {
                *error = at + "'typed' must be true or false";
                return false;
            }
            typed = v == "true";
        } else if (key == "address") {
            uint32_t addr = 0;
            if (!parseNumber(value, &addr) || addr > 0xFFFF) {
                *error = at + "invalid 'address' value '" + value + "' (0-65535, or hex as &7C01 / 0x7C01)";
                return false;
            }
            prog.address = static_cast<uint16_t>(addr);
            hasAddress = true;
            prog.hasAddress = true;
        } else if (key == "slot") {
            *error = at +
                     "'slot:' was removed -- the loader places code by the machine's MODE, the program area "
                     "TITLE selects, and the address. Switch MODE / TITLE in a 'keys:' section before the "
                     "program if needed.";
            return false;
        } else if (key == "length") {
            uint32_t v = 0;
            if (!parseNumber(value, &v)) {
                *error = at + "invalid 'length' value '" + value + "'";
                return false;
            }
            prog.length = v;
            prog.hasLength = true;
            hasLength = true;
        } else if (key == "text") {
            if (!hasInline || value != "|") {
                *error = at + "'text' must be a block scalar ('text: |')";
                return false;
            }
            if (idx >= lines.size() || lines[idx].indent <= blockIndent) {
                *error = at + "'text: |' requires indented content";
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
            *error = at + "unrecognized 'program' field '" + key + "'";
            return false;
        }
    }

    const std::string at = "line " + std::to_string(blockLine) + ": ";
    if (hasFile == hasText) {
        *error = at + "'program:' needs either 'file:' or 'text: |'";
        return false;
    }
    if (hasText) {
        if (hasAddress || hasLength || typed) {
            *error = at + "'text: |' is typed in; 'address', 'length' and 'typed' go with 'file:'";
            return false;
        }
        prog.format = PresetProgram::Format::BasicText;
        *out = prog;
        return true;
    }

    std::vector<uint8_t> bytes;
    if (!readProgramBytes(prog.path, &bytes, error)) return false;
    using programfile::Kind;
    const programfile::ProgramFile file = programfile::classify(bytes);
    const std::string name = std::filesystem::path(prog.path).filename().string();
    switch (file.kind) {
        case Kind::BasicListing:
            if (typed) {
                prog.format = PresetProgram::Format::BasicText;
                prog.text.assign(bytes.begin(), bytes.end());
            } else {
                prog.format = PresetProgram::Format::BasicBinary;
            }
            break;
        case Kind::BasicPC1500:
        case Kind::BasicPC1600:
            if (typed) {
                *error = at + name + " is tokenized BASIC; only a listing can be typed in";
                return false;
            }
            prog.format = PresetProgram::Format::BasicBinary;
            break;
        case Kind::CodeLH5801:
        case Kind::CodeZ80:
        case Kind::Headerless:
            if (typed) {
                *error = at + name + " is machine code; only a BASIC listing can be typed in";
                return false;
            }
            prog.format = PresetProgram::Format::Binary;
            break;
        case Kind::Empty:
            *error = at + name + " is empty";
            return false;
        case Kind::Other:
            *error = at + name + " is " + programfile::describe(file.token) + ", not a program";
            return false;
    }
    if (prog.format != PresetProgram::Format::Binary && (hasAddress || hasLength)) {
        *error = at + "'address' and 'length' go with machine code; " + name +
                 " is BASIC (it loads where the machine's BASIC pointers say)";
        return false;
    }
    *out = prog;
    return true;
}

// A block that is YAML proper (nested maps and lists: `debug:`,
// `bus-rom:`): its lines go to the YAML reader, shifted to column 0 and
// padded so that the reader's line numbers are the file's.
bool parseYamlBlock(const std::vector<RawLine>& lines, size_t& idx, const RawLine& keyLine, const std::string& key,
                    YamlNode* out, std::string* error) {
    if (idx >= lines.size() || lines[idx].indent == 0) {
        *error = "line " + std::to_string(keyLine.lineNo) + ": '" + key + ":' is empty";
        return false;
    }
    const int indent = lines[idx].indent;
    std::string doc;
    int at = 1;
    for (; idx < lines.size() && lines[idx].indent > 0; idx++) {
        if (lines[idx].indent < indent) {
            *error = "line " + std::to_string(lines[idx].lineNo) + ": '" + key +
                     ":' block is indented less than its first line";
            return false;
        }
        for (; at < lines[idx].lineNo; at++) doc += '\n';
        doc += std::string(static_cast<size_t>(lines[idx].indent - indent), ' ') + lines[idx].content + '\n';
        at++;
    }
    if (!parseYaml(doc, out, error)) {
        *error = key + ": " + *error;
        return false;
    }
    return true;
}

// `bus-rom:` -- a list of ROM files on the 60-pin bus (Connector/BusRomCard.hpp):
//   - file: build/ext.bin
//     bank: 7                 # PC-1600 system bus, page B bank 4-7
//   - file: ext1500.bin
//     address: &8000          # PC-1500 connector / PC-1600 LH5803 side
//     me: 1                   # optional: ME1 (default: ME0)
//     pv: 1                   # optional: only while PV (PU) is 0 / 1
//     pu: 0
bool parseBusRoms(const YamlNode& block, const std::filesystem::path& presetDir, std::vector<PresetBusRom>* out,
                  std::string* error) {
    if (!block.isSeq()) {
        *error = "line " + std::to_string(block.line) + ": expected a list of ROMs ('- file: ...')";
        return false;
    }
    // Every value here is a number in the preset's one syntax (parseNumber).
    auto number = [error](const YamlNode& n, const char* key, uint32_t* v) {
        std::string text;
        if (!n.asString(&text, error)) return false;
        if (!parseNumber(text, v)) {
            *error = "line " + std::to_string(n.line) + ": '" + key + "' must be a number, not '" + text + "'";
            return false;
        }
        return true;
    };
    for (const YamlNode& item : block.seq) {
        if (!item.isMap() || !item.has("file")) {
            *error = "line " + std::to_string(item.line) + ": each ROM needs 'file'";
            return false;
        }
        if (!item.requireOnlyKeys({"file", "bank", "address", "me", "pv", "pu"}, error)) return false;
        PresetBusRom rom;
        if (!item.find("file")->asString(&rom.path, error)) return false;
        rom.path = resolvePath(presetDir, rom.path);
        uint32_t v = 0;
        const YamlNode* address = item.find("address");
        if (const YamlNode* n = item.find("bank")) {
            if (!number(*n, "bank", &v)) return false;
            if (v < 4 || v > 7) {
                *error = "line " + std::to_string(n->line) + ": 'bank' must be 4-7 (page B banks on the system bus)";
                return false;
            }
            rom.bank = int(v);
        }
        if (address) {
            if (!number(*address, "address", &v)) return false;
            if (v > 0xFFFF) {
                *error = "line " + std::to_string(address->line) + ": 'address' must be &0000-&FFFF";
                return false;
            }
            rom.address = uint16_t(v);
        }
        if ((rom.bank >= 0) == (address != nullptr)) {
            *error = "line " + std::to_string(item.line) + ": give either 'bank' (PC-1600 system bus) or 'address'";
            return false;
        }
        if (rom.bank >= 0 && (item.has("me") || item.has("pv") || item.has("pu"))) {
            *error = "line " + std::to_string(item.line) + ": 'me', 'pv' and 'pu' go with 'address', not 'bank'";
            return false;
        }
        int me = 0;
        const std::pair<const char*, int*> flags[] = {{"me", &me}, {"pv", &rom.pv}, {"pu", &rom.pu}};
        for (const auto& [flag, dest] : flags)
            if (const YamlNode* n = item.find(flag)) {
                if (!number(*n, flag, &v)) return false;
                if (v != 0 && v != 1) {
                    *error = "line " + std::to_string(n->line) + ": '" + flag + "' must be 0 or 1";
                    return false;
                }
                *dest = int(v);
            }
        rom.me1 = me == 1;
        out->push_back(rom);
    }
    return true;
}

}  // namespace

bool parsePresetFile(const std::string& path, PresetFile* out, std::string* error) {
    std::vector<RawLine> lines;
    if (!readLines(path, &lines, error)) return false;

    std::filesystem::path presetDir = std::filesystem::path(path).parent_path();
    if (presetDir.empty()) presetDir = ".";

    // `format-version` first, before the key list can reject a newer key.
    bool hasFormatVersion = false;
    for (const RawLine& line : lines) {
        std::string key, value;
        bool hasInline;
        if (line.indent != 0 || !splitKeyValue(line.content, &key, &value, &hasInline) || key != "format-version")
            continue;
        const std::string at = "line " + std::to_string(line.lineNo) + ": ";
        uint32_t version = 0;
        if (!parseNumber(value, &version)) {
            *error = at + "'format-version: " + value + "' is not a number";
            return false;
        }
        if (version != kPresetFormatVersion) {
            *error = at + "unsupported preset format-version " + std::to_string(version) + " (this build reads " +
                     std::to_string(kPresetFormatVersion) + ")";
            return false;
        }
        hasFormatVersion = true;
    }
    if (!hasFormatVersion) {
        *error = "'format-version' is required";
        return false;
    }

    std::string modelRom;    // the ROM suffix of `model: NAME:ROM`, lower-cased ("" = none given)
    std::string plotter;     // "" / "ce1600p" / "ce150", ROM suffix stripped
    std::string plotterRom;  // the ROM suffix of `plotter: NAME:ROM`, lower-cased ("" = none given)
    std::string interfaceName;  // "" / "ce158"
    std::string floppy;  // `floppy:` / `floppy-file:` value with any `,A`/`,B` suffix stripped
    int floppySide = 0;  // 0 = A, 1 = B, parsed from that suffix
    bool hasFloppy = false;
    bool floppyIsFile = false;  // `floppy-file:` (a resolved path) rather than `floppy:` (a disk-name)
    bool hasSlot1 = false, hasSlot2 = false;
    std::string hostDrive;  // `host-drive:`, resolved

    // `NAME[:ROM]` -> NAME and ROM (lower-cased); an empty ROM is an error.
    auto splitRom = [](const std::string& value, std::string* name, std::string* rom) {
        const size_t colon = value.find(':');
        *name = value.substr(0, colon);
        *rom = colon == std::string::npos ? "" : value.substr(colon + 1);
        lowerAscii(*rom);
        return colon == std::string::npos || !rom->empty();
    };

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
        const std::string at = "line " + std::to_string(line.lineNo) + ": ";
        static const char* const kKeys[] = {"format-version", "model", "plotter", "interface", "floppy", "floppy-file", "host-drive",
                                            "slot-1", "slot-1-file", "slot-2", "slot-2-file", "program", "keys",
                                            "debug", "bus-rom"};
        if (std::find(std::begin(kKeys), std::end(kKeys), key) == std::end(kKeys)) {
            *error = at + "unrecognized field '" + key + "'";
            return false;
        }
        const bool block = key == "program" || key == "keys" || key == "debug" || key == "bus-rom";
        if (block && hasInline) {
            *error = at + "'" + key + ":' takes a block, not an inline value";
            return false;
        }
        if (!block && !hasInline) {
            *error = at + "'" + key + "' requires a value";
            return false;
        }
        if (key == "format-version") {
            // checked above
        } else if (key == "model") {
            // `model: NAME[:ROM]` -- the ROM revision rides on the model
            // (`PC-1500:A01`, `PC-1600:old`); validated per model below.
            std::string name;
            if (!splitRom(value, &name, &modelRom)) {
                *error = at + "'model: " + value + "' has an empty ROM after ':'";
                return false;
            }
            if (isProductName(name, "PC-1500")) out->model = "PC-1500";
            else if (isProductName(name, "PC-1500A")) out->model = "PC-1500A";
            else if (isProductName(name, "PC-1600")) out->model = "PC-1600";
            else {
                *error = at + "unsupported model '" + name + "' (must be PC-1500, PC-1500A or PC-1600)";
                return false;
            }
        } else if (key == "program") {
            PresetSection section;
            section.kind = PresetSection::Kind::Program;
            if (!parseProgramBlock(lines, idx, presetDir, &section.program, error)) return false;
            out->sections.push_back(std::move(section));
        } else if (key == "keys") {
            PresetSection section;
            section.kind = PresetSection::Kind::Keys;
            if (!parseStepList(lines, idx, presetDir, &section.keys, error)) return false;
            out->sections.push_back(std::move(section));
        } else if (key == "slot-1" || key == "slot-1-file" || key == "slot-2" || key == "slot-2-file") {
            // `slot-N: <module-name>` / `slot-N-file: <path>` -- the memory
            // module in that slot, like `floppy:` / `floppy-file:`.
            const bool slot1 = key[5] == '1';
            bool& seen = slot1 ? hasSlot1 : hasSlot2;
            if (seen) {
                *error = at + "only one of 'slot-" + key[5] + ":' / 'slot-" + key[5] + "-file:' is allowed";
                return false;
            }
            seen = true;
            if (key.size() > 6) (slot1 ? out->slot1ModuleSpecFile : out->slot2ModuleSpecFile) = resolvePath(presetDir, value);
            else (slot1 ? out->slot1ModuleSpecName : out->slot2ModuleSpecName) = value;
        } else if (key == "plotter") {
            // `plotter: NAME[:ROM]` -- only the CE-1600P has a ROM choice.
            std::string name;
            if (!splitRom(value, &name, &plotterRom)) {
                *error = at + "'plotter: " + value + "' has an empty ROM after ':'";
                return false;
            }
            if (isProductName(name, "CE-1600P")) plotter = "ce1600p";
            else if (isProductName(name, "CE-150")) plotter = "ce150";
            else {
                *error = at + "'plotter: " + value + "' is not a known plotter (expected CE-1600P or CE-150)";
                return false;
            }
            if (!plotterRom.empty() && plotter != "ce1600p") {
                *error = at + "'plotter: " + value + "' -- only the CE-1600P has a ROM choice ('plotter: CE-1600P:new|old')";
                return false;
            }
            if (!plotterRom.empty() && plotterRom != "new" && plotterRom != "old") {
                *error = at + "'plotter: " + value + "' -- the CE-1600P ROM must be 'new' or 'old'";
                return false;
            }
        } else if (key == "interface") {
            if (!isProductName(value, "CE-158")) {
                *error = at + "'interface: " + value + "' is not a known interface (expected CE-158)";
                return false;
            }
            interfaceName = "ce158";
        } else if (key == "host-drive") {
            hostDrive = resolvePath(presetDir, value);
        } else if (key == "floppy" || key == "floppy-file") {
            if (hasFloppy) {
                *error = at + "only one of 'floppy:' / 'floppy-file:' is allowed";
                return false;
            }
            hasFloppy = true;
            floppyIsFile = key == "floppy-file";
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
                    *error = at + "'" + key + ": " + value + "' has an invalid side suffix (expected ,A or ,B)";
                    return false;
                }
            }
            if (floppyIsFile) floppy = resolvePath(presetDir, floppy);
        } else {  // `debug:` / `bus-rom:`
            YamlNode yamlBlock;
            if (!parseYamlBlock(lines, idx, line, key, &yamlBlock, error)) return false;
            if (key == "debug") {
                if (!parsePresetDebugBlock(&yamlBlock, presetDir, error)) {
                    *error = "debug: " + *error;
                    return false;
                }
                out->debug = std::move(yamlBlock);
            } else if (!parseBusRoms(yamlBlock, presetDir, &out->busRoms, error)) {
                *error = "bus-rom: " + *error;
                return false;
            }
        }
    }

    if (out->model.empty()) {
        *error = "'model' is required";
        return false;
    }
    out->plotter = plotter;
    out->interfaceName = interfaceName;
    if (out->model == "PC-1600") {
        // `model: PC-1600:new|old` picks the calculator ROM (default new);
        // the CE-1600P ROM (`plotter: CE-1600P:new|old`) is independent.
        out->romVariant = modelRom.empty() ? "new" : modelRom;
        if (out->romVariant != "new" && out->romVariant != "old") {
            *error = "'model: PC-1600:" + modelRom + "' -- the PC-1600 ROM must be 'new' or 'old'";
            return false;
        }
        out->ce1600pRomVariant = plotterRom.empty() ? "new" : plotterRom;
        if (!interfaceName.empty() && plotter == "ce1600p") {
            *error = "'interface: CE-158' cannot be combined with 'plotter: CE-1600P' (the CE-158 does "
                     "not connect to the CE-1600P)";
            return false;
        }
        if (hasFloppy && plotter != "ce1600p") {
            *error = std::string("'") + (floppyIsFile ? "floppy-file" : "floppy") +
                     ":' requires 'plotter: CE-1600P' (the CE-1600F comes with it)";
            return false;
        }
        (floppyIsFile ? out->floppyFile : out->floppy) = floppy;
        out->floppySide = floppySide;
        out->hostDrive = hostDrive;
        return true;
    }

    // PC-1500 / PC-1500A: one slot, no floppy, no host drive, no system bus.
    const std::string model = out->model;
    if (hasSlot2) {
        *error = "'slot-2:' is PC-1600 only -- the " + model + " has one slot ('slot-1:')";
        return false;
    }
    for (const PresetSection& s : out->sections)
        for (const PresetStep& step : s.keys)
            if (step.kind == PresetStep::Kind::SaveAs && step.saveAsTarget != PresetStep::SaveAsTarget::Slot1) {
                *error = "'saveas: slot-2:' / 'saveas: floppy:' are PC-1600 only -- the " + model +
                         " has one slot ('saveas: slot-1:') and no floppy drive";
                return false;
            }
    if (hasFloppy) {
        *error = "'floppy:' / 'floppy-file:' are PC-1600 only (the CE-1600F is a PC-1600 device)";
        return false;
    }
    if (!hostDrive.empty()) {
        *error = "'host-drive:' is PC-1600 only";
        return false;
    }
    for (const PresetBusRom& rom : out->busRoms)
        if (rom.bank >= 0) {
            *error = "bus-rom: 'bank' is the PC-1600 system bus -- a " + model + " ROM takes 'address'";
            return false;
        }
    if (plotter == "ce1600p") {
        *error = "'plotter: CE-1600P' is a PC-1600 device -- use 'plotter: CE-150' on the " + model;
        return false;
    }
    // `model: PC-1500:A01|A03|A04` / `model: PC-1500A:A04` -- which ROM
    // revision; WHERE the file lives is the caller's concern.
    for (char& ch : modelRom) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    out->romVariant = modelRom.empty() ? "A04" : modelRom;
    if (model == "PC-1500A") {
        out->variant = PC1500Variant::PC1500A;
        if (out->romVariant != "A04") {
            *error = "'model: PC-1500A:" + modelRom + "' -- the PC-1500A can only run ROM A04";
            return false;
        }
    } else {
        out->variant = PC1500Variant::PC1500;
        if (out->romVariant != "A01" && out->romVariant != "A03" && out->romVariant != "A04") {
            *error = "'model: PC-1500:" + modelRom + "' -- the PC-1500 ROM must be A01, A03 or A04";
            return false;
        }
    }
    return true;
}
