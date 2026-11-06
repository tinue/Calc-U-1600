#pragma once

#include "Shared/Rgb.hpp"

// ── LCD panel layout ─────────────────────────────────────────────────────
//
// Where a renderer puts the dot matrix and the status indicators of the
// PC-1500/1500A and PC-1600 LCDs, as fractions of the panel, and the panel
// colours. Hand-tuned against the real displays; the Qt LcdWidget paints
// with them, a web renderer uses the same numbers.
namespace shared::lcd {

using shared::Rgb;

// ── PC-1500/1500A layout constants -- hand-tuned to match the real
// device's indicator-bar/dot-matrix proportions ───────────────────────────
inline constexpr double kP15IndicatorBarFraction = 18.0 / 42.0;
inline constexpr double kP15IndicatorFontHeightFraction = 0.277;
inline constexpr double kP15IndicatorTopInsetFraction = 0.01;
inline constexpr double kP15ContentScale = 24.52 / 31.1;
inline constexpr double kP15MatrixTopGapFraction = 0.2;

inline constexpr double kP15BusyX = 0.0053;
inline constexpr double kP15ShiftX = 0.0998;
inline constexpr double kP15JapX = 0.1943;
inline constexpr double kP15SmallX = 0.2572;
inline constexpr double kP15AngleX = 0.3579;
inline constexpr double kP15RunX = 0.5090;
inline constexpr double kP15ProX = 0.6034;
inline constexpr double kP15ReserveX = 0.6664;
inline constexpr double kP15DefX = 0.7922;
inline constexpr double kP15RomanIX = 0.8740;
inline constexpr double kP15RomanIIX = 0.8930;
inline constexpr double kP15RomanIIIX = 0.9245;
inline constexpr double kP15BatteryX = 0.9874;

// ── PC-1600 layout constants -- hand-tuned; fractions are of the panel's
// own width/height directly. ──────────────────────────────────────────────
inline constexpr double kP16IndicatorRowTop = 0.03;
inline constexpr double kP16LabelFontHeightFraction = 26.0 / 337.0; // labelFontSize / canvasSize.height
inline constexpr double kP16RowHeightFraction = kP16LabelFontHeightFraction * 1.15;
inline constexpr double kP16MatrixLeft = 0.05;
inline constexpr double kP16MatrixRight = 0.955;
inline constexpr double kP16MatrixTop = 0.18;
inline constexpr double kP16MatrixBottom = 1.00;
inline constexpr double kP16RowLeft = 0.025;
inline constexpr double kP16RowRight = 0.9205;

struct P16Item { const char* name; double rawX; };
// rawStartX, measured from source-material/Display.png -- fixed reference
// data, remapped below into [kP16RowLeft, kP16RowRight] exactly like
// Layout.startX(for:) does.
inline constexpr P16Item kP16Items[] = {
    {"busy", 0.04837}, {"shift", 0.10587}, {"s", 0.17526}, {"romaji", 0.19389}, {"kana", 0.2502},
    {"small", 0.31047}, {"degrad", 0.39611}, {"run", 0.50436}, {"pro", 0.54401},
    {"reserve", 0.60984}, {"def", 0.72086}, {"roman", 0.76209}, {"ctrl", 0.81562},
    {"batt", 0.87192},
};
inline constexpr double kP16MeasuredLeft = 0.04837;  // kP16Items[0] (busy)
inline constexpr double kP16MeasuredRight = 0.87192; // kP16Items[last] (batt)

inline double p16StartX(double rawX) {
    const double t = (rawX - kP16MeasuredLeft) / (kP16MeasuredRight - kP16MeasuredLeft);
    return kP16RowLeft + t * (kP16RowRight - kP16RowLeft);
}

inline constexpr Rgb kP15BackgroundRgb{150, 158, 142};
inline constexpr Rgb kP16BackgroundRgb{156, 168, 149};
inline constexpr Rgb kInkRgb{0x20, 0x28, 0x18};

} // namespace shared::lcd
