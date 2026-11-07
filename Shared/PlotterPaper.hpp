#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "Connector/AlpsPlotterMechanism.hpp"
#include "Shared/Rgb.hpp"

// ── Plotter paper: geometry, strokes and export size ─────────────────────
//
// What a renderer needs to draw the CE-150 / CE-1600P paper: where a pen
// position lands on the pane, which polylines to stroke in which colour,
// and how large a physical-size export may get. The Qt PlotterPaperWidget
// only turns these into QPainterPaths and a QImage.
namespace shared {

struct PaperPoint {
    double x = 0;
    double y = 0;
};

// Shared geometry for both plotter papers -- CE-150 and CE-1600P differ
// only in paperWidthMM/plottableWidthMM, so this is one parameterized
// struct instead of two hand-duplicated ones.
//
// penX/penY are raw AlpsPlotterMechanism::FlatPoint step-coordinates (NOT
// mm) -- a full motor step is +-2 raw units, so kPlotUnitsPerMM = 1920/190
// (not 960/190): BASIC's X=0..960 plot window maps to 190mm, but the
// mechanism's own raw penX range is double that. Shared by both plotters --
// same physical stepper hardware, not recomputed per paper width.
// Which plotter paper the paper panel shows.
enum class PaperKind { None, CE150, CE1600P };

// "CE-150" / "CE-1600P"; empty for None.
inline const char* paperName(PaperKind kind) {
    return kind == PaperKind::CE150 ? "CE-150" : kind == PaperKind::CE1600P ? "CE-1600P" : "";
}

struct PaperGeometry {
    double paperWidthMM = 0;
    double plottableWidthMM = 0;

    static constexpr double kPlotUnitsPerMM = 1920.0 / 190.0;
    static constexpr double kVerticalInsetPt = 24.0;
    static constexpr double kPhysicalPointsPerMM = 72.0 / 25.4;

    static PaperGeometry ce1600p() { return PaperGeometry{210.0, 190.0}; }
    static PaperGeometry ce150() { return PaperGeometry{56.0, 42.75}; }
    static PaperGeometry forKind(PaperKind kind) { return kind == PaperKind::CE150 ? ce150() : ce1600p(); }

    double sideMarginMM() const { return (paperWidthMM - plottableWidthMM) / 2.0; }

    // The full sheet width always maps to the pane's width -- on-screen this
    // is whatever fits the viewport; for physical export, paneWidthPt is set
    // to physicalPaneWidthPt() so this becomes exactly kPhysicalPointsPerMM.
    double pointsPerMM(double paneWidthPt) const { return paneWidthPt > 0 ? paneWidthPt / paperWidthMM : 0.0; }

    // No Y-flip: increasing penY (paper feeding) maps to larger screen Y --
    // newest output further down, matching "paper rolls out downward."
    // Flipping this axis renders every plotted glyph upside down.
    PaperPoint canvasPoint(std::int32_t penX, std::int32_t penY, std::int32_t penYLowerBound, double paneWidthPt) const {
        const double ppmm = pointsPerMM(paneWidthPt);
        const double x = (sideMarginMM() + penX / kPlotUnitsPerMM) * ppmm;
        const double y = kVerticalInsetPt + (penY - penYLowerBound) / kPlotUnitsPerMM * ppmm;
        return PaperPoint{x, y};
    }

    // Grows exactly with plotted vertical extent plus insets top+bottom;
    // never shrinks below one pane-height (an empty/short plot still fills
    // the viewport). Paper only ever extends downward -- penYLowerBound is
    // always the anchor for the top inset, matching real hardware (paper is
    // physically torn off after printing, it can't un-print upward).
    double contentHeight(double paneHeightPt, std::int32_t penYLower, std::int32_t penYUpper, double paneWidthPt) const {
        const double ppmm = pointsPerMM(paneWidthPt);
        const double spanPt = (penYUpper - penYLower) / kPlotUnitsPerMM * ppmm;
        return std::max(spanPt + 2.0 * kVerticalInsetPt, paneHeightPt);
    }

    // True physical sheet width in Cocoa/PDF points (1pt = 1/72in) -- used
    // for the clipboard export so pasting/printing at 100% matches the real
    // paper's physical size.
    double physicalPaneWidthPt() const { return paperWidthMM * kPhysicalPointsPerMM; }
};

// pen[color % 4] -- AlpsPlotterMechanism::PenColor's raw value order:
// black, blue, dark green, red.
inline constexpr int kPenCount = 4;
inline constexpr std::array<Rgb, kPenCount> kPenColors{{{0, 0, 0}, {0, 0, 255}, {0, 128, 0}, {255, 0, 0}}};
// Strokes are 1pt wide with round caps and joins.
inline constexpr double kPenWidthPt = 1.0;

using Polyline = std::vector<PaperPoint>;
// Per pen colour, the polylines to stroke (a new one starts at every
// FlatPoint::newStroke). Paint colour 0 first, then 1, 2, 3.
using PaperStrokes = std::array<std::vector<Polyline>, kPenCount>;

PaperStrokes buildPaperStrokes(const PaperGeometry& geom, double paneWidthPt,
                               const std::vector<AlpsPlotterMechanism::FlatPoint>& points, std::int32_t penYLower);

// Lowest/highest pen Y in `points`, {0, 0} when empty.
std::pair<std::int32_t, std::int32_t> penYRange(const std::vector<AlpsPlotterMechanism::FlatPoint>& points);

// Size of the physical-size (1200 DPI) export of a sheet with pen Y range
// [penYLower, penYUpper]: the pane in points and the scale to pixels,
// backed off below 1200 DPI for long rolls (GPU texture and total-pixel
// caps). dotsPerMeter carries the real resolution for the image file.
struct PaperExportSize {
    double paneWidthPt = 0;
    double contentHeightPt = 0;
    double scale = 0;  // pixels per point
    int widthPx = 0;
    int heightPx = 0;
    int dotsPerMeter = 0;
};
PaperExportSize paperExportSize(const PaperGeometry& geom, std::int32_t penYLower, std::int32_t penYUpper);

} // namespace shared
