#pragma once
#include <QPointF>
#include <algorithm>
#include <cstdint>

// ── PaperGeometry ─────────────────────────────────────────────────────────
//
// Shared geometry for both plotter papers -- CE-150 and CE-1600P differ
// only in paperWidthMM/plottableWidthMM, so this is one parameterized
// struct instead of two hand-duplicated ones.
//
// penX/penY are raw AlpsPlotterMechanism::FlatPoint step-coordinates (NOT
// mm) -- a full motor step is +-2 raw units, so kPlotUnitsPerMM = 1920/190
// (not 960/190): BASIC's X=0..960 plot window maps to 190mm, but the
// mechanism's own raw penX range is double that. Shared by both plotters --
// same physical stepper hardware, not recomputed per paper width.
struct PaperGeometry {
    double paperWidthMM = 0;
    double plottableWidthMM = 0;

    static constexpr double kPlotUnitsPerMM = 1920.0 / 190.0;
    static constexpr double kVerticalInsetPt = 24.0;
    static constexpr double kPhysicalPointsPerMM = 72.0 / 25.4;

    static PaperGeometry ce1600p() { return PaperGeometry{210.0, 190.0}; }
    static PaperGeometry ce150() { return PaperGeometry{56.0, 42.75}; }

    double sideMarginMM() const { return (paperWidthMM - plottableWidthMM) / 2.0; }

    // The full sheet width always maps to the pane's width -- on-screen this
    // is whatever fits the viewport; for physical export, paneWidthPt is set
    // to physicalPaneWidthPt() so this becomes exactly kPhysicalPointsPerMM.
    double pointsPerMM(double paneWidthPt) const { return paneWidthPt > 0 ? paneWidthPt / paperWidthMM : 0.0; }

    // No Y-flip: increasing penY (paper feeding) maps to larger screen Y --
    // newest output further down, matching "paper rolls out downward."
    // Flipping this axis renders every plotted glyph upside down.
    QPointF canvasPoint(std::int32_t penX, std::int32_t penY, std::int32_t penYLowerBound, double paneWidthPt) const {
        const double ppmm = pointsPerMM(paneWidthPt);
        const double x = (sideMarginMM() + penX / kPlotUnitsPerMM) * ppmm;
        const double y = kVerticalInsetPt + (penY - penYLowerBound) / kPlotUnitsPerMM * ppmm;
        return QPointF(x, y);
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
