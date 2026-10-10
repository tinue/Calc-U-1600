#pragma once
#include <cstddef>
#include <string>
#include <vector>

// ── Box-drawn text tables for the inspector views ────────────────────────
//
// The debug panel shows monospaced text, so the inspector's tables are drawn
// with box characters: a header row, then body rows, optional rules between
// groups, and span rows (a group's title across every column). Column widths
// follow the content; cells are UTF-8 and measured in code points, which is
// right for the box/arrow/dot characters the views use.
//
//   ┌──────┬──────┐
//   │ Name │ Size │
//   ├──────┼──────┤
//   │ Built-in    │   <- span row
//   │ RAM  │  16K │
//   └──────┴──────┘
namespace inspect {

/// Width of `utf8` in code points (each continuation byte is skipped).
size_t displayWidth(const std::string& utf8);

/// `s` padded with spaces to `width` code points (never truncated).
std::string padTo(const std::string& s, size_t width, bool right = false);

class TextTable {
public:
    enum class Align { Left, Right };

    explicit TextTable(std::vector<std::string> headers, std::vector<Align> align = {});

    /// A body row; missing cells are blank, extra cells are dropped.
    void addRow(std::vector<std::string> cells);
    /// A rule (├─┼─┤) before the next row.
    void addRule();
    /// A row whose one cell spans every column, with a rule above it
    /// (unless it is the first body row).
    void addSpan(std::string text);

    bool empty() const { return m_rows.empty(); }

    std::vector<std::string> render() const;

private:
    enum class Kind { Cells, Rule, Span };
    struct Row {
        Kind kind;
        std::vector<std::string> cells;
    };
    std::vector<std::string> m_headers;
    std::vector<Align> m_align;
    std::vector<Row> m_rows;
};

}  // namespace inspect
