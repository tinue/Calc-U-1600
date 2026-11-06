// Shared/PlotterPaper, Shared/KeyLayout/FaceplateLayout.

#include <cmath>
#include <cstdio>
#include <vector>

#include "Shared/KeyLayout/FaceplateLayout.hpp"
#include "Shared/PlotterPaper.hpp"
#include "SharedTestSupport.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;
#define CHECK(cond) SHARED_CHECK(g_pass, g_fail, cond)

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

using FlatPoint = AlpsPlotterMechanism::FlatPoint;

void test_canvas_point() {
    const auto g = shared::PaperGeometry::ce1600p();
    // Pane width == paper width in mm: 1 pt per mm.
    const shared::PaperPoint p = g.canvasPoint(0, 0, 0, 210.0);
    CHECK(near(p.x, 10.0));  // (210 - 190) / 2 side margin
    CHECK(near(p.y, shared::PaperGeometry::kVerticalInsetPt));
    const shared::PaperPoint q = g.canvasPoint(1920, 1920, 0, 210.0);
    CHECK(near(q.x, 200.0));  // 1920 raw units = 190 mm
    CHECK(near(q.y, 24.0 + 190.0));
    CHECK(near(g.contentHeight(1000.0, 0, 0, 210.0), 1000.0));
    CHECK(near(g.contentHeight(0.0, 0, 1920, 210.0), 190.0 + 48.0));
}

void test_strokes() {
    const auto g = shared::PaperGeometry::ce150();
    const std::vector<FlatPoint> pts = {
        {0, 0, 0, 1}, {10, 0, 0, 0}, {10, 10, 0, 0},  // black: one polyline of 3
        {0, 5, 3, 0},                                 // red: first point opens a line
        {20, 20, 0, 1}, {30, 20, 0, 0},               // black: second polyline
    };
    const auto strokes = shared::buildPaperStrokes(g, 100.0, pts, 0);
    CHECK(strokes[0].size() == 2);
    CHECK(strokes[0][0].size() == 3);
    CHECK(strokes[0][1].size() == 2);
    CHECK(strokes[1].empty() && strokes[2].empty());
    CHECK(strokes[3].size() == 1 && strokes[3][0].size() == 1);
    const auto [lo, hi] = shared::penYRange(pts);
    CHECK(lo == 0 && hi == 20);
    CHECK(shared::penYRange({}).first == 0 && shared::penYRange({}).second == 0);
}

void test_export_size() {
    const auto g = shared::PaperGeometry::ce1600p();
    const shared::PaperExportSize s = shared::paperExportSize(g, 0, 0);
    CHECK(near(s.scale, 1200.0 / 72.0));
    CHECK(s.widthPx == 9921);
    CHECK(s.heightPx == 800);
    CHECK(s.dotsPerMeter == 47244);
    // A very long roll backs off below 1200 DPI to stay under the caps.
    const shared::PaperExportSize big = shared::paperExportSize(g, 0, 200000);
    CHECK(big.scale < 1200.0 / 72.0);
    CHECK(big.heightPx <= 16000);
    CHECK(static_cast<double>(big.widthPx) * big.heightPx <= 64.0e6 * 1.001);
}

void test_faceplate_fit() {
    const KeyLayout::Layout& l = KeyLayout::layoutFor(shared::Model::PC1600);
    CHECK(&KeyLayout::layoutFor(shared::Model::PC1500) == &KeyLayout::layoutFor(shared::Model::PC1500A));
    CHECK(!KeyLayout::fitLayout(l, 0, 100).art.valid());

    // A pane twice as wide as the art's aspect: letterboxed horizontally.
    const int h = 400;
    const int w = static_cast<int>(2 * h * l.imageWidth / l.imageHeight);
    const KeyLayout::FittedLayout f = KeyLayout::fitLayout(l, w, h);
    CHECK(f.art.h == h);
    CHECK(f.art.y == 0);
    CHECK(f.art.x > 0 && f.art.x + f.art.w <= w);
    CHECK(f.keys.size() == l.keyCount);
    CHECK(f.lcd.valid());

    // Every key's centre hits that key (or one listed earlier that overlaps).
    bool allHit = true;
    for (std::size_t i = 0; i < f.keys.size(); ++i) {
        const auto& r = f.keys[i];
        if (!r.valid()) continue;
        const int idx = KeyLayout::hitTest(f, r.x + r.w / 2, r.y + r.h / 2);
        if (idx < 0 || idx > static_cast<int>(i)) allHit = false;
    }
    CHECK(allHit);
    CHECK(KeyLayout::hitTest(f, 0, 0) == -1);  // letterbox margin
    CHECK(KeyLayout::keyIndex(l, "enter") >= 0);
    CHECK(KeyLayout::keyIndex(l, "no-such-key") == -1);
    CHECK(KeyLayout::heightForWidth(l, 1000) == static_cast<int>(1000 * (l.imageHeight / l.imageWidth)));
}

} // namespace

int run_shared_geometry_tests() {
    test_canvas_point();
    test_strokes();
    test_export_size();
    test_faceplate_fit();
    std::printf("shared_geometry_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
