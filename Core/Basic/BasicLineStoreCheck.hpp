#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ── "Did the line editor store the line just typed?" ────────────────────
//
// Shared by the PC-1500 and PC-1600 BASIC typers. Both ROMs keep the
// program as sorted in-RAM line records (see BasicBinaryImage.hpp):
//
//   [lineNo hi][lineNo lo][len][content ...][0x0D]   len = content + 1
//
// and move BASPRG_END whenever a line is added, deleted or resized. A line
// that replaces one of the same tokenised length leaves BASPRG_END put and
// only rewrites that record, so a pointer compare alone reports it as not
// stored. capture() therefore also keeps the bytes of the record whose
// number the typed line starts with (if one is resident); changed() compares
// BASPRG_END first and only falls back to that one record. Cost is one walk
// over the record headers, not a copy of the whole program.
//
// Re-typing a line identical to the stored one changes nothing and is
// reported as not stored -- same as before this check existed.

namespace basic {

class LineStoreCheck {
public:
    /// `peek(addr)` reads one byte; `start`/`end` are the program area
    /// [BASPRG_ST, BASPRG_END) in the address space `peek` uses.
    template <class Peek>
    static LineStoreCheck capture(Peek&& peek, uint32_t start, uint32_t end, const std::string& typedLine) {
        LineStoreCheck c;
        c.m_end = end;
        int lineNo = leadingLineNumber(typedLine);
        if (lineNo < 0) return c;
        uint32_t a = start;
        while (a + 3 <= end && a + 3 <= 0x10000) {
            const int no = (peek(a) << 8) | peek(a + 1);
            const uint32_t size = 3u + peek(a + 2);
            if (no > lineNo) break; // records are sorted by line number
            if (no == lineNo) {
                c.m_lineAddr = a;
                for (uint32_t i = 0; i < size && a + i <= 0xFFFF; i++) c.m_lineBytes.push_back(peek(a + i));
                break;
            }
            a += size;
        }
        return c;
    }

    /// True if the program changed since capture(): BASPRG_END moved, or
    /// the typed line's resident record was rewritten in place.
    template <class Peek>
    bool changed(Peek&& peek, uint32_t endNow) const {
        if (endNow != m_end) return true;
        for (size_t i = 0; i < m_lineBytes.size(); i++)
            if (peek(m_lineAddr + static_cast<uint32_t>(i)) != m_lineBytes[i]) return true;
        return false;
    }

private:
    // The typed line's leading line number (leading blanks skipped), or -1.
    static int leadingLineNumber(const std::string& line) {
        size_t i = 0;
        while (i < line.size() && line[i] == ' ') i++;
        int n = -1;
        for (; i < line.size() && line[i] >= '0' && line[i] <= '9'; i++) {
            n = (n < 0 ? 0 : n) * 10 + (line[i] - '0');
            if (n > 0xFFFF) return -1;
        }
        return n;
    }

    uint32_t m_end = 0;
    uint32_t m_lineAddr = 0;
    std::vector<uint8_t> m_lineBytes; // empty: no resident record with that number
};

}  // namespace basic
