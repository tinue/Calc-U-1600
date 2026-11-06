#include "Shared/PlotterPaper.hpp"

#include <cmath>

namespace shared {

namespace {
constexpr double kDpiScale = 1200.0 / 72.0;   // target 1200 DPI (72pt/in base)
constexpr double kMaxTextureDimPx = 16000.0;  // GPU texture size cap
// Total-pixel cap for the export. Copy holds the image, the pasteboard's
// bitmap and its TIFF/PNG encodings at once, so 64 MP (256 MB a copy) is
// already a lot; uncapped, a long roll ran past 2 GB. Full 1200 DPI up to
// ~136 mm of CE-1600P paper, scaled down beyond that. The narrow CE-150
// roll hits the long-side cap first (~339 mm) and stays under this one.
constexpr double kMaxExportPixels = 64.0e6;
} // namespace

PaperStrokes buildPaperStrokes(const PaperGeometry& geom, double paneWidthPt,
                               const std::vector<AlpsPlotterMechanism::FlatPoint>& points, std::int32_t penYLower) {
    PaperStrokes strokes;
    for (const auto& pt : points) {
        auto& lines = strokes[pt.color % kPenCount];
        if (pt.newStroke || lines.empty()) lines.emplace_back();
        lines.back().push_back(geom.canvasPoint(pt.x, pt.y, penYLower, paneWidthPt));
    }
    return strokes;
}

std::pair<std::int32_t, std::int32_t> penYRange(const std::vector<AlpsPlotterMechanism::FlatPoint>& points) {
    if (points.empty()) return {0, 0};
    const auto [lo, hi] = std::minmax_element(points.begin(), points.end(),
                                              [](const auto& a, const auto& b) { return a.y < b.y; });
    return {lo->y, hi->y};
}

PaperExportSize paperExportSize(const PaperGeometry& geom, std::int32_t penYLower, std::int32_t penYUpper) {
    PaperExportSize out;
    out.paneWidthPt = geom.physicalPaneWidthPt();
    out.contentHeightPt = geom.contentHeight(0, penYLower, penYUpper, out.paneWidthPt);
    const double longestDimPt = std::max(out.paneWidthPt, out.contentHeightPt);
    const double areaPt = out.paneWidthPt * out.contentHeightPt;
    double scale = kDpiScale;
    if (longestDimPt > 0) scale = std::min(scale, kMaxTextureDimPx / longestDimPt);
    if (areaPt > 0) scale = std::min(scale, std::sqrt(kMaxExportPixels / areaPt));
    out.scale = scale;
    out.widthPx = std::max(1, static_cast<int>(std::lround(out.paneWidthPt * scale)));
    out.heightPx = std::max(1, static_cast<int>(std::lround(out.contentHeightPt * scale)));
    out.dotsPerMeter = static_cast<int>(std::lround(scale * 72.0 / 0.0254));
    return out;
}

} // namespace shared
