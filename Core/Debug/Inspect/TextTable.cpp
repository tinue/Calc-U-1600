#include "TextTable.hpp"

#include <algorithm>

namespace inspect {

size_t displayWidth(const std::string& utf8) {
    size_t n = 0;
    for (unsigned char c : utf8)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

std::string padTo(const std::string& s, size_t width, bool right) {
    const size_t w = displayWidth(s);
    if (w >= width) return s;
    const std::string fill(width - w, ' ');
    return right ? fill + s : s + fill;
}

TextTable::TextTable(std::vector<std::string> headers, std::vector<Align> align)
    : m_headers(std::move(headers)), m_align(std::move(align)) {
    m_align.resize(m_headers.size(), Align::Left);
}

void TextTable::addRow(std::vector<std::string> cells) {
    cells.resize(m_headers.size());
    m_rows.push_back({Kind::Cells, std::move(cells)});
}

void TextTable::addRule() {
    if (!m_rows.empty() && m_rows.back().kind != Kind::Rule) m_rows.push_back({Kind::Rule, {}});
}

void TextTable::addSpan(std::string text) {
    addRule();
    m_rows.push_back({Kind::Span, {std::move(text)}});
}

std::vector<std::string> TextTable::render() const {
    const size_t n = m_headers.size();
    std::vector<size_t> width(n);
    for (size_t i = 0; i < n; ++i) width[i] = displayWidth(m_headers[i]);
    for (const Row& r : m_rows)
        if (r.kind == Kind::Cells)
            for (size_t i = 0; i < n; ++i) width[i] = std::max(width[i], displayWidth(r.cells[i]));
    // A span row wider than the columns widens the last one.
    size_t inner = 0;
    for (size_t i = 0; i < n; ++i) inner += width[i] + 3;
    inner -= 1; // "│ a │ b │": every column takes w + 3, minus the leading "│"
    for (const Row& r : m_rows)
        if (r.kind == Kind::Span) {
            const size_t need = displayWidth(r.cells[0]) + 2;
            if (need > inner) {
                width[n - 1] += need - inner;
                inner = need;
            }
        }

    auto line = [&](const char* left, const char* mid, const char* right) {
        std::string s = left;
        for (size_t i = 0; i < n; ++i) {
            for (size_t k = 0; k < width[i] + 2; ++k) s += "\xE2\x94\x80"; // ─
            s += i + 1 < n ? mid : right;
        }
        return s;
    };
    auto cells = [&](const std::vector<std::string>& c) {
        std::string s = "\xE2\x94\x82"; // │
        for (size_t i = 0; i < n; ++i)
            s += " " + padTo(c[i], width[i], m_align[i] == Align::Right) + " \xE2\x94\x82";
        return s;
    };

    std::vector<std::string> out;
    out.push_back(line("\xE2\x94\x8C", "\xE2\x94\xAC", "\xE2\x94\x90")); // ┌ ┬ ┐
    out.push_back(cells(m_headers));
    out.push_back(line("\xE2\x94\x9C", "\xE2\x94\xBC", "\xE2\x94\xA4")); // ├ ┼ ┤
    for (size_t k = 0; k < m_rows.size(); ++k) {
        const Row& r = m_rows[k];
        switch (r.kind) {
            case Kind::Cells: out.push_back(cells(r.cells)); break;
            case Kind::Rule:
                if (k > 0 && k + 1 < m_rows.size())
                    out.push_back(line("\xE2\x94\x9C", "\xE2\x94\xBC", "\xE2\x94\xA4"));
                break;
            case Kind::Span:
                out.push_back("\xE2\x94\x82 " + padTo(r.cells[0], inner - 2) + " \xE2\x94\x82");
                break;
        }
    }
    out.push_back(line("\xE2\x94\x94", "\xE2\x94\xB4", "\xE2\x94\x98")); // └ ┴ ┘
    return out;
}

}  // namespace inspect
