#include "PlotterPaperWidget.hpp"
#include "ChromeColors.hpp"
#include "MachineController.hpp"
#include "MacClipboardImage.h"

#include <QClipboard>
#include <QEvent>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include <cmath>

namespace {
constexpr double kFollowThresholdPt = 64.0;

// Shared by on-screen paint and clipboard export: one QPainterPath per pen
// color from shared::buildPaperStrokes(), each stroked with a 1pt-wide
// round-capped/joined pen.
void paintPaper(QPainter& painter, const shared::PaperGeometry& geom, double paneWidthPt, double contentHeightPt,
                 const std::vector<AlpsPlotterMechanism::FlatPoint>& points, std::int32_t penYLower) {
    painter.fillRect(QRectF(0, 0, paneWidthPt, contentHeightPt), Qt::white);
    if (points.empty()) return;

    const shared::PaperStrokes strokes = shared::buildPaperStrokes(geom, paneWidthPt, points, penYLower);
    for (int i = 0; i < shared::kPenCount; ++i) {
        if (strokes[i].empty()) continue;
        QPainterPath path;
        for (const shared::Polyline& line : strokes[i]) {
            path.moveTo(line.front().x, line.front().y);
            for (std::size_t k = 1; k < line.size(); ++k) path.lineTo(line[k].x, line[k].y);
        }
        const shared::Rgb rgb = shared::kPenColors[i];
        QPen pen(QColor(rgb.r, rgb.g, rgb.b), shared::kPenWidthPt);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.drawPath(path);
    }
}
} // namespace

// Custom-painted plot surface -- its sizeHint drives QScrollArea's content
// height (contentHeight grows as more is plotted).
class PlotterPaperWidget::PlotArea : public QWidget {
public:
    explicit PlotArea(PlotterPaperWidget* owner) : m_owner(owner) {}

    QSize sizeHint() const override {
        const int w = std::max(1, width());
        const auto [lower, upper] = m_owner->m_penYRange;
        const double h = m_owner->m_geometry.contentHeight(0, lower, upper, w);
        return QSize(w, static_cast<int>(std::ceil(h)));
    }

    // QScrollArea (widgetResizable) only grows its widget past the viewport
    // up to the widget's *minimum* size hint, not its sizeHint -- without
    // this the paper never exceeds the viewport and there is nothing to
    // scroll. Height only: a width minimum would pin the paper at a stale
    // width and it would stop fitting the pane.
    QSize minimumSizeHint() const override { return QSize(0, sizeHint().height()); }

protected:
    // The content height depends on the width (fixed-width paper scaled to
    // the pane), so a resize changes what the scroll area needs.
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        updateGeometry();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        paintPaper(painter, m_owner->m_geometry, width(), height(), m_owner->m_points, m_owner->m_penYRange.first);
    }

private:
    PlotterPaperWidget* m_owner;
};

PlotterPaperWidget::PlotterPaperWidget(MachineController* controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Header -- matches DebugPanel's "INSPECTOR" header exactly (same
    // ChromeColors, same font/padding), showing which plotter is attached
    // instead of a fixed label.
    m_header = new QLabel(this);
    m_header->setAlignment(Qt::AlignCenter);
    QFont headerFont = m_header->font();
    headerFont.setBold(true);
    headerFont.setPointSize(headerFont.pointSize() - 1);
    m_header->setFont(headerFont);
    outer->addWidget(m_header);

    m_plotArea = new PlotArea(this);
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidget(m_plotArea);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // No visible bar (the wheel/touchpad scrolls); the range still exists.
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Never take keyboard focus from MainWindow (which owns physical-
    // keyboard typing for the calculator) -- same convention as
    // ControlBar's widgets.
    m_scrollArea->setFocusPolicy(Qt::NoFocus);
    outer->addWidget(m_scrollArea, /*stretch=*/1);

    // Button bar -- underneath the paper, matching DebugPanel's TRACE/LOG
    // row in size/padding and using the same ChromeColors::buttonBarBackground
    // (only one row here, so the bar itself ends up shorter than DebugPanel's
    // two-row bar -- by design, not a mismatch).
    m_buttonBar = new QWidget(this);
    auto* buttonBarLayout = new QHBoxLayout(m_buttonBar);
    buttonBarLayout->setContentsMargins(10, 8, 10, 8);
    buttonBarLayout->setSpacing(8);
    m_copyButton = new QPushButton(tr("Copy"), m_buttonBar);
    m_copyButton->setToolTip(tr("Copy the paper to the clipboard as an image."));
    m_copyButton->setFocusPolicy(Qt::NoFocus);
    m_copyButton->setObjectName(QStringLiteral("paper.copy"));
    m_cutButton = new QPushButton(tr("Cut"), m_buttonBar);
    m_cutButton->setToolTip(tr("Copy the paper, then tear it off (clears the plot)."));
    m_cutButton->setFocusPolicy(Qt::NoFocus);
    m_cutButton->setObjectName(QStringLiteral("paper.cut"));
    buttonBarLayout->addWidget(m_copyButton);
    buttonBarLayout->addWidget(m_cutButton);
    buttonBarLayout->addStretch(1);
    outer->addWidget(m_buttonBar);

    connect(m_copyButton, &QPushButton::clicked, this, &PlotterPaperWidget::copyToClipboard);
    connect(m_cutButton, &QPushButton::clicked, this, &PlotterPaperWidget::cutPaper);

    applyChrome();
    updateButtonsEnabled();
}

void PlotterPaperWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
        applyChrome();
    }
}

void PlotterPaperWidget::applyChrome() {
    const ChromeColors c = ChromeColors::forWidget(this);

    m_header->setText(m_kind == Kind::CE1600P ? tr("CE-1600P") : tr("CE-150"));
    m_header->setStyleSheet(ChromeStyle::header(c));

    m_buttonBar->setStyleSheet(ChromeStyle::buttonBar(c));

    // Pill-styled, matching DebugPanel's TRACE/LOG buttons in size/shape
    // (same padding/radius/colors, via ChromeStyle::pillCore) -- with an
    // explicit :pressed rule that wins over :hover (both match while the
    // mouse is held down over the button, and Qt's stylesheet cascade
    // favors whichever selector has equal-or-greater specificity;
    // :hover:!disabled alone would otherwise shadow a plain :pressed rule)
    // so a click gives clearly visible "goes down" feedback -- there is no
    // other visible change from Copy/Cut, unlike DebugPanel's dump buttons
    // where the log output itself is the feedback.
    const QString buttonStyle = ChromeStyle::pillPushButton(c);
    m_copyButton->setStyleSheet(buttonStyle);
    m_cutButton->setStyleSheet(buttonStyle);
}

void PlotterPaperWidget::setKind(Kind kind) {
    m_kind = kind;
    m_geometry = kind == Kind::CE1600P ? shared::PaperGeometry::ce1600p() : shared::PaperGeometry::ce150();
    setPoints({});
    m_lastRevision = UINT64_MAX;
    m_plotArea->updateGeometry();
    m_plotArea->update();
    applyChrome();
    updateButtonsEnabled();
}

void PlotterPaperWidget::setPoints(std::vector<AlpsPlotterMechanism::FlatPoint> points) {
    m_points = std::move(points);
    m_penYRange = shared::penYRange(m_points);
}

bool PlotterPaperWidget::isNearBottom() const {
    QScrollBar* bar = m_scrollArea->verticalScrollBar();
    return (bar->maximum() - bar->value()) <= kFollowThresholdPt;
}

void PlotterPaperWidget::scrollToBottom() {
    QScrollBar* bar = m_scrollArea->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void PlotterPaperWidget::updateButtonsEnabled() {
    const bool hasPoints = !m_points.empty();
    m_copyButton->setEnabled(hasPoints);
    m_cutButton->setEnabled(hasPoints);
}

void PlotterPaperWidget::onFrameTick() {
    const std::uint64_t rev = m_kind == Kind::CE1600P ? m_controller->ce1600pPlotRevision()
                                                         : m_controller->ce150PlotRevision();
    if (rev == m_lastRevision) return;
    m_lastRevision = rev;

    const bool wasNearBottom = isNearBottom();
    setPoints(m_kind == Kind::CE1600P ? m_controller->ce1600pPlotPoints() : m_controller->ce150PlotPoints());
    m_plotArea->updateGeometry();
    m_plotArea->update();
    updateButtonsEnabled();
    if (wasNearBottom) scrollToBottom();
}

QImage PlotterPaperWidget::renderPaperImage() const {
    if (m_points.empty()) return {};

    const auto [lower, upper] = m_penYRange;
    const shared::PaperExportSize size = shared::paperExportSize(m_geometry, lower, upper);
    // RGBA8888: the byte order macSetClipboardImage() hands to Cocoa, so
    // its conversion is a no-op rather than a second full-size copy.
    QImage image(size.widthPx, size.heightPx, QImage::Format_RGBA8888);
    QPainter painter(&image);
    // Deliberately no anti-aliasing: at 1200 DPI the strokes are ~16 px wide,
    // and the soft edges only made the printout look worse.
    painter.scale(size.scale, size.scale);
    paintPaper(painter, m_geometry, size.paneWidthPt, size.contentHeightPt, m_points, lower);
    painter.end();

    // Embed true DPI so a direct file save (or a non-macOS clipboard path)
    // carries physical size too, even when the scale backed off from
    // 1200 DPI to respect the caps (long rolls).
    image.setDotsPerMeterX(size.dotsPerMeter);
    image.setDotsPerMeterY(size.dotsPerMeter);
    return image;
}

void PlotterPaperWidget::copyToClipboard() {
    const QImage image = renderPaperImage();
    if (image.isNull()) return;
    const auto [lower, upper] = m_penYRange;
    const shared::PaperExportSize size = shared::paperExportSize(m_geometry, lower, upper);

#ifdef Q_OS_MACOS
    // Qt's cross-platform QClipboard::setImage() ignores both
    // devicePixelRatio and dotsPerMeter when converting to NSImage on
    // macOS (confirmed empirically) -- go straight to Cocoa instead, so
    // Preview/printing see the correct physical size.
    if (macSetClipboardImage(image, size.paneWidthPt, size.contentHeightPt)) return;
#endif
    QGuiApplication::clipboard()->setImage(image);
}

void PlotterPaperWidget::cutPaper() {
    copyToClipboard();
    if (m_kind == Kind::CE1600P) m_controller->clearCE1600PPaper();
    else m_controller->clearCE150Paper();
    setPoints({});
    m_lastRevision = UINT64_MAX;
    m_plotArea->updateGeometry();
    m_plotArea->update();
    updateButtonsEnabled();
}
