#pragma once

// The debugger's expression evaluator as it was before expressions were
// compiled (a parse-and-evaluate pass): the reference the compiled form is
// checked against in debug_target_tests.cpp. Test-only.

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../Debug/DebugExpression.hpp"

namespace legacy {

using debug::ExpressionContext;
using debug::ExpressionResult;

// Recursive-descent parser that evaluates as it goes. The first error
// sticks; later results are ignored.
class Parser {
public:
    Parser(const std::string& text, const ExpressionContext& ctx) : m_s(text), m_ctx(ctx) {}

    ExpressionResult run() {
        ExpressionResult r;
        skipSpace();
        if (m_pos >= m_s.size()) { r.error = "empty expression"; return r; }
        const int64_t v = parseOr();
        skipSpace();
        if (m_error.empty() && m_pos < m_s.size()) fail("unexpected '" + m_s.substr(m_pos, 1) + "'");
        if (!m_error.empty()) { r.error = m_error; return r; }
        r.ok = true;
        r.value = v;
        return r;
    }

private:
    const std::string& m_s;
    const ExpressionContext& m_ctx;
    size_t m_pos = 0;
    std::string m_error;

    void fail(const std::string& msg) { if (m_error.empty()) m_error = msg; }
    void skipSpace() { while (m_pos < m_s.size() && std::isspace(static_cast<unsigned char>(m_s[m_pos]))) m_pos++; }
    bool accept(const char* op) {
        skipSpace();
        size_t n = 0;
        while (op[n]) n++;
        if (m_s.compare(m_pos, n, op) != 0) return false;
        // "&" must not match "&&", "|" not "||", "<" not "<<" or "<=", ...
        const char next = m_pos + n < m_s.size() ? m_s[m_pos + n] : '\0';
        if (n == 1) {
            const char c = op[0];
            if ((c == '&' || c == '|' || c == '<' || c == '>' || c == '=') && next == c) return false;
            if ((c == '<' || c == '>' || c == '!') && next == '=') return false;
        }
        m_pos += n;
        return true;
    }

    int64_t parseOr() {
        int64_t v = parseAnd();
        while (accept("||")) { const int64_t r = parseAnd(); v = (v || r) ? 1 : 0; }
        return v;
    }
    int64_t parseAnd() {
        int64_t v = parseBitOr();
        while (accept("&&")) { const int64_t r = parseBitOr(); v = (v && r) ? 1 : 0; }
        return v;
    }
    int64_t parseBitOr() {
        int64_t v = parseBitXor();
        while (accept("|")) v |= parseBitXor();
        return v;
    }
    int64_t parseBitXor() {
        int64_t v = parseBitAnd();
        while (accept("^")) v ^= parseBitAnd();
        return v;
    }
    int64_t parseBitAnd() {
        int64_t v = parseEquality();
        while (accept("&")) v &= parseEquality();
        return v;
    }
    int64_t parseEquality() {
        int64_t v = parseRelational();
        for (;;) {
            if (accept("==")) v = (v == parseRelational()) ? 1 : 0;
            else if (accept("!=")) v = (v != parseRelational()) ? 1 : 0;
            else return v;
        }
    }
    int64_t parseRelational() {
        int64_t v = parseShift();
        for (;;) {
            if (accept("<=")) v = (v <= parseShift()) ? 1 : 0;
            else if (accept(">=")) v = (v >= parseShift()) ? 1 : 0;
            else if (accept("<")) v = (v < parseShift()) ? 1 : 0;
            else if (accept(">")) v = (v > parseShift()) ? 1 : 0;
            else return v;
        }
    }
    int64_t parseShift() {
        int64_t v = parseAdditive();
        for (;;) {
            if (accept("<<")) v = int64_t(uint64_t(v) << (parseAdditive() & 63));
            else if (accept(">>")) v >>= (parseAdditive() & 63);
            else return v;
        }
    }
    int64_t parseAdditive() {
        int64_t v = parseMultiplicative();
        for (;;) {
            if (accept("+")) v += parseMultiplicative();
            else if (accept("-")) v -= parseMultiplicative();
            else return v;
        }
    }
    int64_t parseMultiplicative() {
        int64_t v = parseUnary();
        for (;;) {
            if (accept("*")) v *= parseUnary();
            else if (accept("/") || accept("%")) {
                const bool div = m_s[m_pos - 1] == '/';
                const int64_t r = parseUnary();
                if (r == 0) { fail("division by zero"); return 0; }
                v = div ? v / r : v % r;
            } else return v;
        }
    }
    int64_t parseUnary() {
        if (accept("!")) return parseUnary() ? 0 : 1;
        if (accept("~")) return ~parseUnary();
        if (accept("-")) return -parseUnary();
        if (accept("+")) return parseUnary();
        return parsePrimary();
    }

    int64_t readMemory(bool word, bool me1) {
        const int64_t addr = parseOr();
        if (!accept("]")) { fail("missing ']'"); return 0; }
        if (!m_ctx.readByte) { fail("memory not available"); return 0; }
        const uint16_t a = uint16_t(addr);
        uint8_t b0 = 0, b1 = 0;
        if (!m_ctx.readByte(a, me1, &b0) || (word && !m_ctx.readByte(uint16_t(a + 1), me1, &b1))) {
            fail("memory at " + std::to_string(a) + " is not readable");
            return 0;
        }
        if (!word) return b0;
        return m_ctx.bigEndian ? (int64_t(b0) << 8) | b1 : (int64_t(b1) << 8) | b0;
    }

    static int hexDigit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        c = char(std::tolower(static_cast<unsigned char>(c)));
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    }

    int64_t parseHex() {
        int64_t v = 0;
        size_t start = m_pos;
        while (m_pos < m_s.size() && hexDigit(m_s[m_pos]) >= 0) v = v * 16 + hexDigit(m_s[m_pos++]);
        if (m_pos == start) fail("missing hex digits");
        return v;
    }

    int64_t parsePrimary() {
        skipSpace();
        if (m_pos >= m_s.size()) { fail("unexpected end of expression"); return 0; }
        const char c = m_s[m_pos];
        if (c == '(') {
            m_pos++;
            const int64_t v = parseOr();
            if (!accept(")")) fail("missing ')'");
            return v;
        }
        if (c == '[') { m_pos++; return readMemory(false, false); }
        if (c == '#' && m_pos + 1 < m_s.size() && m_s[m_pos + 1] == '[') { m_pos += 2; return readMemory(false, true); }
        if ((c == 'w' || c == 'W') && m_pos + 1 < m_s.size() && m_s[m_pos + 1] == '[') { m_pos += 2; return readMemory(true, false); }
        if (c == '$' || c == '&') { m_pos++; return parseHex(); }
        if (c == '0' && m_pos + 1 < m_s.size() && (m_s[m_pos + 1] == 'x' || m_s[m_pos + 1] == 'X')) { m_pos += 2; return parseHex(); }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            int64_t v = 0;
            while (m_pos < m_s.size() && std::isdigit(static_cast<unsigned char>(m_s[m_pos]))) v = v * 10 + (m_s[m_pos++] - '0');
            return v;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
            std::string name;
            while (m_pos < m_s.size() && (std::isalnum(static_cast<unsigned char>(m_s[m_pos])) || m_s[m_pos] == '_' ||
                                          m_s[m_pos] == '.' || m_s[m_pos] == '$' || m_s[m_pos] == '\''))
                name += m_s[m_pos++];
            std::string lower = name;
            for (char& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            int64_t v = 0;
            if (m_ctx.lookup && (m_ctx.lookup(lower, &v) || (lower != name && m_ctx.lookup(name, &v)))) return v;
            fail("unknown name '" + name + "'");
            return 0;
        }
        fail(std::string("unexpected '") + c + "'");
        return 0;
    }
};


inline ExpressionResult evaluate(const std::string& text, const ExpressionContext& ctx) { return Parser(text, ctx).run(); }

// BreakpointTable's hit-condition check and log interpolation as they were
// before being parsed once.
inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

inline bool hitConditionMet(const std::string& hitCondition, int hits, bool* valid) {
    std::string t = trim(hitCondition);
    if (valid) *valid = true;
    if (t.empty()) return true;
    std::string op = "==";
    for (const char* candidate : {"==", ">=", "<=", "!=", ">", "<", "%", "="}) {
        const size_t n = std::char_traits<char>::length(candidate);
        if (t.compare(0, n, candidate) == 0) { op = candidate; t = trim(t.substr(n)); break; }
    }
    char* end = nullptr;
    const long long n = std::strtoll(t.c_str(), &end, 0);
    if (t.empty() || !end || *end != '\0') {
        if (valid) *valid = false;
        return true;
    }
    if (op == "==" || op == "=") return hits == n;
    if (op == ">=") return hits >= n;
    if (op == "<=") return hits <= n;
    if (op == "!=") return hits != n;
    if (op == ">") return hits > n;
    if (op == "<") return hits < n;
    return n > 0 && hits % n == 0;
}

inline std::string valueText(int64_t v) {
    char buf[24];
    if (v < 0) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
    else if (v <= 0xFF) std::snprintf(buf, sizeof buf, "0x%02llX", static_cast<unsigned long long>(v));
    else std::snprintf(buf, sizeof buf, "0x%04llX", static_cast<unsigned long long>(v));
    return buf;
}

inline std::string interpolateLog(const std::string& message, const ExpressionContext& ctx) {
    std::string out;
    for (size_t i = 0; i < message.size(); i++) {
        if (message[i] != '{') { out += message[i]; continue; }
        const size_t close = message.find('}', i);
        if (close == std::string::npos) { out += message.substr(i); break; }
        const ExpressionResult r = legacy::evaluate(message.substr(i + 1, close - i - 1), ctx);
        out += r.ok ? valueText(r.value) : "{" + r.error + "}";
        i = close;
    }
    return out;
}

} // namespace legacy
