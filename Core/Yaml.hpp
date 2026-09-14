#pragma once
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

// ── Minimal YAML-subset reader ─────────────────────────────────────────
//
// Just enough YAML for the memory-card definition format
// (docs/Memory-Card-Definition-Format.md). NOT general YAML. Supported:
//   * indent-based block mappings (`key: value`, or `key:` + nested block)
//   * `- ` block sequences; an item is a scalar, a flow collection, or a
//     mapping (`- key: v` then more keys aligned under the first)
//   * flow sequences `[a, b, c]` and flow mappings `{ k: v, k: v }`
//   * `#` comments -- a whole-line comment or a ` #...` trailing comment
//     (honoured outside quotes and outside `[]`/`{}`)
//   * single- and double-quoted scalars (no escape processing, matching
//     Core/PC1500/PresetFile.cpp) and bare scalars
//   * `key: |` block scalars (literal, common-indent stripped)
// Not supported: anchors/aliases, tags, `>`, multiple documents, complex
// keys, flow scalars spanning lines. Tabs are rejected, as in PresetFile.
//
// Error convention matches the rest of Core: `bool` return + `std::string*
// error` (1-based line number where possible), never exceptions.

struct YamlNode {
    enum class Type { Null, Scalar, Sequence, Map };
    Type type = Type::Null;
    std::string scalar;  // Type::Scalar (raw, quotes still on if quoted)
    std::vector<YamlNode> seq;
    std::vector<std::pair<std::string, YamlNode>> map;  // ordered
    int line = 0;                                       // 1-based, for errors

    bool isNull() const { return type == Type::Null; }
    bool isScalar() const { return type == Type::Scalar; }
    bool isSeq() const { return type == Type::Sequence; }
    bool isMap() const { return type == Type::Map; }

    const YamlNode* find(const std::string& key) const {
        if (type != Type::Map) return nullptr;
        for (const auto& kv : map)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    bool has(const std::string& key) const { return find(key) != nullptr; }

    bool asString(std::string* out, std::string* error) const;
    bool asInt(long* out, std::string* error) const;  // decimal or 0x..
    bool asBool(bool* out, std::string* error) const;

    // Every key present in this map must appear in `allowed`.
    bool requireOnlyKeys(std::initializer_list<const char*> allowed,
                         std::string* error) const;
};

bool parseYaml(const std::string& text, YamlNode* out, std::string* error);

// ── implementation ───────────────────────────────────────────────────────

namespace yaml_detail {

inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

inline std::string errAt(int line, const std::string& msg) {
    return "line " + std::to_string(line) + ": " + msg;
}

// Drop a trailing `#` comment that sits outside quotes and outside a flow
// collection, and that is either at the start or preceded by whitespace.
inline std::string stripComment(const std::string& s) {
    bool inS = false, inD = false;
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (inS) {
            if (c == '\'') inS = false;
        } else if (inD) {
            if (c == '"') inD = false;
        } else if (c == '\'') {
            inS = true;
        } else if (c == '"') {
            inD = true;
        } else if (c == '[' || c == '{') {
            depth++;
        } else if (c == ']' || c == '}') {
            if (depth > 0) depth--;
        } else if (c == '#' && depth == 0 && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            return s.substr(0, i);
        }
    }
    return s;
}

inline std::string unquote(const std::string& in) {
    std::string s = trim(in);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\'')))
        return s.substr(1, s.size() - 2);
    return s;
}

// Position of the ": " (or trailing ":") that marks this line as a mapping
// entry, ignoring colons inside quotes or flow collections. -1 if none.
inline int mappingColon(const std::string& s) {
    bool inS = false, inD = false;
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (inS) {
            if (c == '\'') inS = false;
        } else if (inD) {
            if (c == '"') inD = false;
        } else if (c == '\'') {
            inS = true;
        } else if (c == '"') {
            inD = true;
        } else if (c == '[' || c == '{') {
            depth++;
        } else if (c == ']' || c == '}') {
            if (depth > 0) depth--;
        } else if (c == ':' && depth == 0) {
            if (i + 1 == s.size() || s[i + 1] == ' ') return static_cast<int>(i);
        }
    }
    return -1;
}

struct Line {
    int indent = 0;
    int lineNo = 0;
    std::string content;  // comment-stripped, right-trimmed; never empty
    std::string raw;      // original (for block scalars)
};

inline bool tokenize(const std::string& text, std::vector<Line>* out, std::string* error) {
    size_t pos = 0;
    int lineNo = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string raw = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (nl == std::string::npos) pos = text.size() + 1;
        else pos = nl + 1;
        lineNo++;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        if (raw.find('\t') != std::string::npos) {
            *error = errAt(lineNo, "tab character (use spaces)");
            return false;
        }
        std::string noComment = stripComment(raw);
        // right-trim
        size_t end = noComment.find_last_not_of(' ');
        std::string content = (end == std::string::npos) ? "" : noComment.substr(0, end + 1);
        if (trim(content).empty()) continue;  // blank / comment-only
        int indent = 0;
        while (indent < static_cast<int>(content.size()) && content[indent] == ' ') indent++;
        Line ln;
        ln.indent = indent;
        ln.lineNo = lineNo;
        ln.content = content.substr(indent);
        ln.raw = raw;
        out->push_back(std::move(ln));
    }
    return true;
}

// Forward decls.
bool parseFlow(const std::string& s, YamlNode* out, std::string* error, int lineNo);
bool parseNode(const std::vector<Line>& L, size_t& i, int parentIndent, YamlNode* out,
               std::string* error);

inline bool splitFlowItems(const std::string& body, std::vector<std::string>* out) {
    bool inS = false, inD = false;
    int depth = 0;
    std::string cur;
    for (char c : body) {
        if (inS) {
            cur += c;
            if (c == '\'') inS = false;
        } else if (inD) {
            cur += c;
            if (c == '"') inD = false;
        } else if (c == '\'') {
            inS = true;
            cur += c;
        } else if (c == '"') {
            inD = true;
            cur += c;
        } else if (c == '[' || c == '{') {
            depth++;
            cur += c;
        } else if (c == ']' || c == '}') {
            depth--;
            cur += c;
        } else if (c == ',' && depth == 0) {
            out->push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    std::string last = trim(cur);
    if (!last.empty()) out->push_back(last);
    return depth == 0 && !inS && !inD;
}

inline bool parseFlow(const std::string& in, YamlNode* out, std::string* error, int lineNo) {
    std::string s = trim(in);
    if (s.empty()) {
        out->type = YamlNode::Type::Null;
        out->line = lineNo;
        return true;
    }
    if (s.front() == '[') {
        if (s.back() != ']') {
            *error = errAt(lineNo, "unterminated flow sequence");
            return false;
        }
        out->type = YamlNode::Type::Sequence;
        out->line = lineNo;
        std::vector<std::string> items;
        if (!splitFlowItems(s.substr(1, s.size() - 2), &items)) {
            *error = errAt(lineNo, "malformed flow sequence");
            return false;
        }
        for (const auto& it : items) {
            YamlNode child;
            if (!parseFlow(it, &child, error, lineNo)) return false;
            out->seq.push_back(std::move(child));
        }
        return true;
    }
    if (s.front() == '{') {
        if (s.back() != '}') {
            *error = errAt(lineNo, "unterminated flow mapping");
            return false;
        }
        out->type = YamlNode::Type::Map;
        out->line = lineNo;
        std::vector<std::string> items;
        if (!splitFlowItems(s.substr(1, s.size() - 2), &items)) {
            *error = errAt(lineNo, "malformed flow mapping");
            return false;
        }
        for (const auto& it : items) {
            int c = mappingColon(it);
            if (c < 0) {
                *error = errAt(lineNo, "flow mapping entry '" + it + "' has no ':'");
                return false;
            }
            std::string key = unquote(it.substr(0, c));
            YamlNode child;
            if (!parseFlow(trim(it.substr(c + 1)), &child, error, lineNo)) return false;
            out->map.emplace_back(key, std::move(child));
        }
        return true;
    }
    out->type = YamlNode::Type::Scalar;
    out->scalar = s;  // keep quotes; coercions strip them
    out->line = lineNo;
    return true;
}

// Collect a `|` block scalar: every following line indented past `keyIndent`.
inline void parseBlockScalar(const std::vector<Line>& L, size_t& i, int keyIndent,
                             YamlNode* out) {
    out->type = YamlNode::Type::Scalar;
    out->line = (i < L.size()) ? L[i].lineNo : 0;
    int common = -1;
    std::vector<std::string> body;
    while (i < L.size() && L[i].indent > keyIndent) {
        if (common < 0 || L[i].indent < common) common = L[i].indent;
        body.push_back(L[i].raw);
        i++;
    }
    if (common < 0) common = 0;
    std::string joined;
    for (size_t k = 0; k < body.size(); ++k) {
        std::string t = body[k].size() >= static_cast<size_t>(common) ? body[k].substr(common)
                                                                      : body[k];
        joined += t;
        if (k + 1 < body.size()) joined += '\n';
    }
    out->scalar = joined;
}

inline bool parseMapBody(const std::vector<Line>& L, size_t& i, int mapIndent, YamlNode* out,
                         std::string* error) {
    out->type = YamlNode::Type::Map;
    out->line = L[i].lineNo;
    while (i < L.size() && L[i].indent == mapIndent) {
        const Line& ln = L[i];
        if (ln.content[0] == '-') {
            *error = errAt(ln.lineNo, "sequence item where a mapping key was expected");
            return false;
        }
        int c = mappingColon(ln.content);
        if (c < 0) {
            *error = errAt(ln.lineNo, "expected 'key: value'");
            return false;
        }
        std::string key = unquote(ln.content.substr(0, c));
        std::string rest = trim(ln.content.substr(c + 1));
        for (const auto& kv : out->map) {
            if (kv.first == key) {
                *error = errAt(ln.lineNo, "duplicate key '" + key + "'");
                return false;
            }
        }
        YamlNode value;
        if (rest == "|") {
            i++;
            parseBlockScalar(L, i, mapIndent, &value);
        } else if (!rest.empty()) {
            i++;
            if (!parseFlow(rest, &value, error, ln.lineNo)) return false;
        } else {
            i++;
            if (i < L.size() && L[i].indent > mapIndent) {
                if (!parseNode(L, i, mapIndent, &value, error)) return false;
            } else {
                value.type = YamlNode::Type::Null;
                value.line = ln.lineNo;
            }
        }
        out->map.emplace_back(std::move(key), std::move(value));
    }
    return true;
}

inline bool parseSeq(const std::vector<Line>& L, size_t& i, int seqIndent, YamlNode* out,
                     std::string* error) {
    out->type = YamlNode::Type::Sequence;
    out->line = L[i].lineNo;
    while (i < L.size() && L[i].indent == seqIndent && !L[i].content.empty() &&
           L[i].content[0] == '-') {
        const Line& ln = L[i];
        std::string rest = (ln.content.size() > 1) ? trim(ln.content.substr(1)) : "";
        int keyCol = seqIndent + 2;  // column of the first char after "- "
        YamlNode item;
        if (rest.empty()) {
            i++;
            if (i < L.size() && L[i].indent > seqIndent) {
                if (!parseNode(L, i, seqIndent, &item, error)) return false;
            } else {
                item.type = YamlNode::Type::Null;
                item.line = ln.lineNo;
            }
        } else if (mappingColon(rest) >= 0) {
            // Inline first mapping entry, then aligned continuation keys.
            item.type = YamlNode::Type::Map;
            item.line = ln.lineNo;
            int c = mappingColon(rest);
            std::string key = unquote(rest.substr(0, c));
            std::string vrest = trim(rest.substr(c + 1));
            YamlNode value;
            if (vrest == "|") {
                i++;
                parseBlockScalar(L, i, keyCol, &value);
            } else if (!vrest.empty()) {
                i++;
                if (!parseFlow(vrest, &value, error, ln.lineNo)) return false;
            } else {
                i++;
                if (i < L.size() && L[i].indent > keyCol) {
                    if (!parseNode(L, i, keyCol, &value, error)) return false;
                } else {
                    value.type = YamlNode::Type::Null;
                    value.line = ln.lineNo;
                }
            }
            item.map.emplace_back(std::move(key), std::move(value));
            if (i < L.size() && L[i].indent == keyCol && L[i].content[0] != '-') {
                YamlNode more;
                if (!parseMapBody(L, i, keyCol, &more, error)) return false;
                for (auto& kv : more.map) {
                    for (const auto& ex : item.map) {
                        if (ex.first == kv.first) {
                            *error = errAt(more.line, "duplicate key '" + kv.first + "'");
                            return false;
                        }
                    }
                    item.map.push_back(std::move(kv));
                }
            }
        } else {
            i++;
            if (!parseFlow(rest, &item, error, ln.lineNo)) return false;
        }
        out->seq.push_back(std::move(item));
    }
    return true;
}

inline bool parseNode(const std::vector<Line>& L, size_t& i, int parentIndent, YamlNode* out,
                      std::string* error) {
    if (i >= L.size()) {
        out->type = YamlNode::Type::Null;
        return true;
    }
    int indent = L[i].indent;
    if (indent <= parentIndent) {
        out->type = YamlNode::Type::Null;
        return true;
    }
    if (!L[i].content.empty() && L[i].content[0] == '-' &&
        (L[i].content.size() == 1 || L[i].content[1] == ' ')) {
        return parseSeq(L, i, indent, out, error);
    }
    return parseMapBody(L, i, indent, out, error);
}

}  // namespace yaml_detail

inline bool parseYaml(const std::string& text, YamlNode* out, std::string* error) {
    std::vector<yaml_detail::Line> lines;
    if (!yaml_detail::tokenize(text, &lines, error)) return false;
    if (lines.empty()) {
        out->type = YamlNode::Type::Null;
        return true;
    }
    if (lines.front().indent != 0) {
        *error = yaml_detail::errAt(lines.front().lineNo, "unexpected indentation at document start");
        return false;
    }
    size_t i = 0;
    if (!yaml_detail::parseNode(lines, i, -1, out, error)) return false;
    if (i != lines.size()) {
        *error = yaml_detail::errAt(lines[i].lineNo, "unexpected content (bad indentation?)");
        return false;
    }
    return true;
}

inline bool YamlNode::asString(std::string* out, std::string* error) const {
    if (type != Type::Scalar) {
        *error = yaml_detail::errAt(line, "expected a scalar string");
        return false;
    }
    *out = yaml_detail::unquote(scalar);
    return true;
}

inline bool YamlNode::asInt(long* out, std::string* error) const {
    if (type != Type::Scalar) {
        *error = yaml_detail::errAt(line, "expected an integer");
        return false;
    }
    std::string s = yaml_detail::unquote(scalar);
    if (s.empty()) {
        *error = yaml_detail::errAt(line, "empty integer");
        return false;
    }
    int base = 10;
    size_t start = 0;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') {
        neg = (s[0] == '-');
        start = 1;
    }
    if (s.size() >= start + 2 && s[start] == '0' && (s[start + 1] == 'x' || s[start + 1] == 'X')) {
        base = 16;
        start += 2;
    }
    if (start >= s.size()) {
        *error = yaml_detail::errAt(line, "malformed integer '" + s + "'");
        return false;
    }
    char* endp = nullptr;
    long v = std::strtol(s.c_str() + start, &endp, base);
    if (!endp || *endp != '\0') {
        *error = yaml_detail::errAt(line, "malformed integer '" + s + "'");
        return false;
    }
    *out = neg ? -v : v;
    return true;
}

inline bool YamlNode::asBool(bool* out, std::string* error) const {
    if (type == Type::Scalar) {
        std::string s = yaml_detail::unquote(scalar);
        if (s == "true") { *out = true; return true; }
        if (s == "false") { *out = false; return true; }
    }
    *error = yaml_detail::errAt(line, "expected true or false");
    return false;
}

inline bool YamlNode::requireOnlyKeys(std::initializer_list<const char*> allowed,
                                      std::string* error) const {
    if (type != Type::Map) return true;
    for (const auto& kv : map) {
        bool ok = false;
        for (const char* a : allowed)
            if (kv.first == a) { ok = true; break; }
        if (!ok) {
            *error = yaml_detail::errAt(line, "unknown key '" + kv.first + "'");
            return false;
        }
    }
    return true;
}
