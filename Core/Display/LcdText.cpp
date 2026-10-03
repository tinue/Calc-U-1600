#include "LcdText.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>

namespace {

const char* const kUnparsed = "\xEF\xBF\xBD"; // U+FFFD

uint8_t rowMask(int cellHeight) { return static_cast<uint8_t>((1u << cellHeight) - 1u); }

LcdCell masked(LcdCell cell, const LcdFont& font) {
    for (int c = 0; c < 8; ++c) cell[c] = c < font.cellWidth ? uint8_t(cell[c] & rowMask(font.cellHeight)) : 0;
    return cell;
}

LcdCell inverted(const LcdCell& cell, const LcdFont& font) {
    LcdCell out{};
    for (int c = 0; c < font.cellWidth; ++c) out[c] = uint8_t(~cell[c] & rowMask(font.cellHeight));
    return out;
}

void appendCode(std::string& out, uint8_t code) {
    if (code == '\\') {
        out += "\\\\";
    } else if (code >= 0x20 && code < 0x7F) {
        out += static_cast<char>(code);
    } else {
        char hex[8];
        std::snprintf(hex, sizeof(hex), "\\x%02X", code);
        out += hex;
    }
}

} // namespace

LcdText parseLcdText(const LcdBitmap& bitmap, const LcdFont& font, const std::optional<LcdCursor>& cursor) {
    LcdText result;
    result.poweredOn = bitmap.poweredOn;
    if (cursor && cursor->row >= 0) {
        result.cursorRow = cursor->row;
        result.cursorCol = cursor->col;
    }

    std::map<LcdCell, uint8_t> byCell;
    for (const auto& [code, cell] : font.glyphs) byCell.emplace(masked(cell, font), code);
    std::vector<LcdCell> cursors;
    for (const LcdCell& c : font.cursorShapes) cursors.push_back(masked(c, font));

    const int textRows = bitmap.rows / font.cellHeight;
    const int textCols = bitmap.cols / font.cellWidth;
    const LcdCell blank{};
    for (int tr = 0; tr < textRows; ++tr) {
        std::string line;
        for (int tc = 0; tc < textCols; ++tc) {
            LcdCell cell{};
            if (bitmap.poweredOn) {
                for (int c = 0; c < font.cellWidth; ++c) {
                    for (int r = 0; r < font.cellHeight; ++r) {
                        const int x = tc * font.cellWidth + c, y = tr * font.cellHeight + r;
                        if (bitmap.pixels[static_cast<std::size_t>(y) * bitmap.cols + x]) cell[c] |= uint8_t(1u << r);
                    }
                }
            }
            if (cell == blank) {
                line += ' ';
                continue;
            }
            // The ROMs draw the cursor over the cell: decode what they saved.
            const bool cursorCell = cursor && (cursor->row < 0 ? result.cursorRow < 0
                                                               : tr == cursor->row && tc == cursor->col);
            bool cursorShape = false;
            for (const LcdCell& c : cursors) cursorShape = cursorShape || c == cell;
            if (cursorCell && cursorShape) {
                result.cursorRow = tr;
                result.cursorCol = tc;
                cell = masked(cursor->under, font);
                if (cell == blank) {
                    line += ' ';
                    continue;
                }
            }
            if (auto it = byCell.find(cell); it != byCell.end()) {
                appendCode(line, it->second);
                continue;
            }
            if (auto it = byCell.find(inverted(cell, font)); it != byCell.end()) {
                appendCode(line, it->second);
                ++result.reverseCells;
                continue;
            }
            line += kUnparsed;
            ++result.unparsed;
        }
        // Trailing blanks carry no information and make `expect:` fussy.
        line.erase(line.find_last_not_of(' ') + 1);
        result.rows.push_back(line);
    }
    return result;
}

std::string LcdText::text() const {
    std::string out;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i) out += '\n';
        out += rows[i];
    }
    return out;
}

bool LcdText::contains(const std::string& needle) const {
    for (const std::string& row : rows)
        if (row.find(needle) != std::string::npos) return true;
    return false;
}

std::string LcdText::report() const {
    std::string out = poweredOn ? text() : "(display off)";
    out += "\nstatus:";
    for (const std::string& s : status) out += " " + s;
    if (unparsed) out += "\nunparsed: " + std::to_string(unparsed);
    return out + "\n";
}

bool writeLcdTextReport(const LcdText& text, const std::string& path, std::string* error) {
    const std::string report = text.report();
    if (path == "-") {
        std::fwrite(report.data(), 1, report.size(), stdout);
        return true;
    }
    std::FILE* fh = std::fopen(path.c_str(), "wb");
    if (!fh || std::fwrite(report.data(), 1, report.size(), fh) != report.size()) {
        *error = std::strerror(errno);
        if (fh) std::fclose(fh);
        return false;
    }
    if (std::fclose(fh) != 0) {
        *error = std::strerror(errno);
        return false;
    }
    return true;
}
