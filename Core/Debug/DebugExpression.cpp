#include "DebugExpression.hpp"

#include <cctype>

namespace debug {

// Recursive-descent compiler to a postfix program. Operands are emitted in
// the order they are parsed and an operator after its operands, so running
// the program evaluates everything in the order a parse-and-evaluate pass
// would. The first syntax error becomes a Fail op and ends the program.
class ExpressionCompiler {
public:
    explicit ExpressionCompiler(const std::string& text) : m_s(text) {}

    CompiledExpression compile() {
        skipSpace();
        if (m_pos >= m_s.size()) {
            fail("empty expression");
            return std::move(m_out);
        }
        parseOr();
        skipSpace();
        if (m_pos < m_s.size()) fail("unexpected '" + m_s.substr(m_pos, 1) + "'");
        return std::move(m_out);
    }

private:
    using Code = CompiledExpression::Code;

    const std::string& m_s;
    size_t m_pos = 0;
    bool m_failed = false;
    CompiledExpression m_out;

    void emit(Code code, char sub = 0, int64_t value = 0) {
        if (!m_failed) m_out.m_ops.push_back({code, sub, value});
    }
    int64_t addString(std::string s) {
        m_out.m_strings.push_back(std::move(s));
        return int64_t(m_out.m_strings.size() - 1);
    }
    void fail(const std::string& msg) {
        if (m_failed) return;
        emit(Code::Fail, 0, addString(msg));
        m_failed = true;
    }
    void binary(char op) { emit(Code::Binary, op); }

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

    // Binary operator codes: 'O' ||, 'A' &&, '|', '^', '&', 'E' ==, 'N' !=,
    // 'l' <=, 'g' >=, '<', '>', 'L' <<, 'R' >>, '+', '-', '*', '/', '%'.
    void parseOr() {
        parseAnd();
        while (accept("||")) { parseAnd(); binary('O'); }
    }
    void parseAnd() {
        parseBitOr();
        while (accept("&&")) { parseBitOr(); binary('A'); }
    }
    void parseBitOr() {
        parseBitXor();
        while (accept("|")) { parseBitXor(); binary('|'); }
    }
    void parseBitXor() {
        parseBitAnd();
        while (accept("^")) { parseBitAnd(); binary('^'); }
    }
    void parseBitAnd() {
        parseEquality();
        while (accept("&")) { parseEquality(); binary('&'); }
    }
    void parseEquality() {
        parseRelational();
        for (;;) {
            if (accept("==")) { parseRelational(); binary('E'); }
            else if (accept("!=")) { parseRelational(); binary('N'); }
            else return;
        }
    }
    void parseRelational() {
        parseShift();
        for (;;) {
            if (accept("<=")) { parseShift(); binary('l'); }
            else if (accept(">=")) { parseShift(); binary('g'); }
            else if (accept("<")) { parseShift(); binary('<'); }
            else if (accept(">")) { parseShift(); binary('>'); }
            else return;
        }
    }
    void parseShift() {
        parseAdditive();
        for (;;) {
            if (accept("<<")) { parseAdditive(); binary('L'); }
            else if (accept(">>")) { parseAdditive(); binary('R'); }
            else return;
        }
    }
    void parseAdditive() {
        parseMultiplicative();
        for (;;) {
            if (accept("+")) { parseMultiplicative(); binary('+'); }
            else if (accept("-")) { parseMultiplicative(); binary('-'); }
            else return;
        }
    }
    void parseMultiplicative() {
        parseUnary();
        for (;;) {
            if (accept("*")) { parseUnary(); binary('*'); }
            else if (accept("/") || accept("%")) {
                const char op = m_s[m_pos - 1];
                parseUnary();
                binary(op);
            } else return;
        }
    }
    void parseUnary() {
        if (accept("!")) { parseUnary(); emit(Code::Unary, '!'); return; }
        if (accept("~")) { parseUnary(); emit(Code::Unary, '~'); return; }
        if (accept("-")) { parseUnary(); emit(Code::Unary, '-'); return; }
        if (accept("+")) { parseUnary(); return; }
        parsePrimary();
    }

    void memory(char kind) {
        parseOr();
        if (!accept("]")) { fail("missing ']'"); return; }
        emit(Code::Mem, kind);
    }

    static int hexDigit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        c = char(std::tolower(static_cast<unsigned char>(c)));
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    }

    void parseHex() {
        int64_t v = 0;
        const size_t start = m_pos;
        while (m_pos < m_s.size() && hexDigit(m_s[m_pos]) >= 0) v = v * 16 + hexDigit(m_s[m_pos++]);
        if (m_pos == start) fail("missing hex digits");
        emit(Code::Const, 0, v);
    }

    void parsePrimary() {
        skipSpace();
        if (m_pos >= m_s.size()) { fail("unexpected end of expression"); return; }
        const char c = m_s[m_pos];
        if (c == '(') {
            m_pos++;
            parseOr();
            if (!accept(")")) fail("missing ')'");
            return;
        }
        if (c == '[') { m_pos++; memory('b'); return; }
        if (c == '#' && m_pos + 1 < m_s.size() && m_s[m_pos + 1] == '[') { m_pos += 2; memory('#'); return; }
        if ((c == 'w' || c == 'W') && m_pos + 1 < m_s.size() && m_s[m_pos + 1] == '[') { m_pos += 2; memory('w'); return; }
        if (c == '$' || c == '&') { m_pos++; parseHex(); return; }
        if (c == '0' && m_pos + 1 < m_s.size() && (m_s[m_pos + 1] == 'x' || m_s[m_pos + 1] == 'X')) { m_pos += 2; parseHex(); return; }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            int64_t v = 0;
            while (m_pos < m_s.size() && std::isdigit(static_cast<unsigned char>(m_s[m_pos]))) v = v * 10 + (m_s[m_pos++] - '0');
            emit(Code::Const, 0, v);
            return;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
            std::string name;
            while (m_pos < m_s.size() && (std::isalnum(static_cast<unsigned char>(m_s[m_pos])) || m_s[m_pos] == '_' ||
                                          m_s[m_pos] == '.' || m_s[m_pos] == '$' || m_s[m_pos] == '\''))
                name += m_s[m_pos++];
            std::string lower = name;
            for (char& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            const int64_t index = addString(name);
            addString(lower);
            emit(Code::Name, 0, index);
            return;
        }
        fail(std::string("unexpected '") + c + "'");
    }
};

CompiledExpression CompiledExpression::compile(const std::string& text) { return ExpressionCompiler(text).compile(); }

ExpressionResult CompiledExpression::run(const ExpressionContext& ctx) const {
    ExpressionResult r;
    std::vector<int64_t> stack;
    stack.reserve(8);
    auto pop = [&stack] {
        const int64_t v = stack.back();
        stack.pop_back();
        return v;
    };
    for (const Op& op : m_ops) {
        switch (op.code) {
            case Code::Const:
                stack.push_back(op.value);
                break;
            case Code::Name: {
                const std::string& name = m_strings[size_t(op.value)];
                const std::string& lower = m_strings[size_t(op.value) + 1];
                int64_t v = 0;
                if (!ctx.lookup || !(ctx.lookup(lower, &v) || (lower != name && ctx.lookup(name, &v)))) {
                    r.error = "unknown name '" + name + "'";
                    return r;
                }
                stack.push_back(v);
                break;
            }
            case Code::Mem: {
                const uint16_t a = uint16_t(pop());
                if (!ctx.readByte) {
                    r.error = "memory not available";
                    return r;
                }
                const bool word = op.sub == 'w', me1 = op.sub == '#';
                uint8_t b0 = 0, b1 = 0;
                if (!ctx.readByte(a, me1, &b0) || (word && !ctx.readByte(uint16_t(a + 1), me1, &b1))) {
                    r.error = "memory at " + std::to_string(a) + " is not readable";
                    return r;
                }
                stack.push_back(!word ? b0 : ctx.bigEndian ? (int64_t(b0) << 8) | b1 : (int64_t(b1) << 8) | b0);
                break;
            }
            case Code::Unary: {
                const int64_t v = pop();
                stack.push_back(op.sub == '!' ? (v ? 0 : 1) : op.sub == '~' ? ~v : -v);
                break;
            }
            case Code::Binary: {
                const int64_t b = pop(), a = pop();
                int64_t v = 0;
                switch (op.sub) {
                    case 'O': v = (a || b) ? 1 : 0; break;
                    case 'A': v = (a && b) ? 1 : 0; break;
                    case '|': v = a | b; break;
                    case '^': v = a ^ b; break;
                    case '&': v = a & b; break;
                    case 'E': v = a == b ? 1 : 0; break;
                    case 'N': v = a != b ? 1 : 0; break;
                    case 'l': v = a <= b ? 1 : 0; break;
                    case 'g': v = a >= b ? 1 : 0; break;
                    case '<': v = a < b ? 1 : 0; break;
                    case '>': v = a > b ? 1 : 0; break;
                    case 'L': v = int64_t(uint64_t(a) << (b & 63)); break;
                    case 'R': v = a >> (b & 63); break;
                    case '+': v = a + b; break;
                    case '-': v = a - b; break;
                    case '*': v = a * b; break;
                    case '/':
                    case '%':
                        if (b == 0) {
                            r.error = "division by zero";
                            return r;
                        }
                        v = op.sub == '/' ? a / b : a % b;
                        break;
                }
                stack.push_back(v);
                break;
            }
            case Code::Fail:
                r.error = m_strings[size_t(op.value)];
                return r;
        }
    }
    r.ok = true;
    r.value = stack.empty() ? 0 : stack.back();
    return r;
}

ExpressionResult evaluate(const std::string& text, const ExpressionContext& ctx) {
    return CompiledExpression::compile(text).run(ctx);
}

} // namespace debug
