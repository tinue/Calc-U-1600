#include "Shared/LcdLayout.hpp"

#include <string_view>

#include "Display/LcdText.hpp"
#include "Shared/MachineSession.hpp"

namespace shared::lcd {

namespace {

bool lit(const DisplayFrame& frame, std::string_view name) {
    for (const auto& [label, on] : frame.statusSymbols)
        if (label == name) return on;
    return false;
}

double p16X(std::string_view item) {
    for (const auto& i : kP16Items)
        if (item == i.name) return p16StartX(i.rawX);
    return 0.0;
}

std::string angleLegend(const DisplayFrame& frame) {
    // DE/G/RAD read as one word (DE+G = DEG, G+RAD = GRAD).
    return lcdAngleLegend(lit(frame, "DE"), lit(frame, "G"), lit(frame, "RAD"));
}

std::vector<Indicator> pc1500(const DisplayFrame& frame) {
    std::vector<Indicator> out;
    const auto text = [&](const char* flag, double x, const char* caption) {
        if (lit(frame, flag)) out.push_back({caption, x});
    };
    text("BUSY", kP15BusyX, "BUSY");
    text("SHIFT", kP15ShiftX, "SHIFT");
    text("KANA", kP15JapX, "JPN");
    text("SMALL", kP15SmallX, "SMALL");
    text("RUN", kP15RunX, "RUN");
    text("PRO", kP15ProX, "PRO");
    text("RESERVE", kP15ReserveX, "RESERVE");
    text("DEF", kP15DefX, "DEF");
    text("I", kP15RomanIX, "I");
    text("II", kP15RomanIIX, "II");
    text("III", kP15RomanIIIX, "III");
    if (std::string angle = angleLegend(frame); !angle.empty()) out.push_back({std::move(angle), kP15AngleX});
    out.push_back({{}, kP15BatteryX, 1.0, Indicator::Style::Dot});
    return out;
}

std::vector<Indicator> pc1600(const DisplayFrame& frame) {
    std::vector<Indicator> out;
    const auto add = [&](const char* flag, const char* item, std::string caption, double scale = 1.0,
                         Indicator::Style style = Indicator::Style::Text) {
        if (lit(frame, flag)) out.push_back({std::move(caption), p16X(item), scale, style});
    };
    add("BUSY", "busy", "BUSY");
    add("SHIFT", "shift", "SHIFT");
    add("S", "s", "S", 1.0, Indicator::Style::Inverted);
    // The romaji->kana caption shares the window after S and is printed
    // small on the glass. The "kana" position is an estimate (the other
    // positions are measured) that keeps both parts clear of SMALL.
    add("ROMAJI", "romaji", "ローマ字", 0.75);
    add("KANA", "kana", "→カナ", 0.75);
    add("SMALL", "small", "SMALL");
    add("RUN", "run", "RUN");
    add("PRO", "pro", "PRO");
    add("RESERVE", "reserve", "RESERVE");
    add("DEF", "def", "DEF");
    add("CTRL", "ctrl", "CTRL");
    add("BATT", "batt", "BATT", 1.0, Indicator::Style::Inverted);
    if (std::string angle = angleLegend(frame); !angle.empty()) out.push_back({std::move(angle), p16X("degrad")});
    for (const char* roman : {"I", "II", "III"}) {
        if (lit(frame, roman)) {
            out.push_back({roman, p16X("roman")});
            break;
        }
    }
    return out;
}

} // namespace

std::vector<Indicator> indicators(Model model, const DisplayFrame& frame) {
    return model == Model::PC1600 ? pc1600(frame) : pc1500(frame);
}

} // namespace shared::lcd
