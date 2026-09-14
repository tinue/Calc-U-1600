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

#include <algorithm>
#include <cmath>

namespace {
constexpr double kFollowThresholdPt = 64.0;
constexpr double kDpiScale = 600.0 / 72.0;     // target 600 DPI (72pt/in base)
constexpr double kMaxTextureDimPx = 16000.0;   // GPU texture size cap

// pen[color % 4] -- AlpsPlotterMechanism::PenColor's raw value order.
const QColor kPenColors[4] = {Qt::black, Qt::blue, Qt::darkGreen, Qt::red};

// Shared by on-screen paint and clipboard export: builds one QPainterPath
// per pen color (moveTo on newStroke, else lineTo) and strokes each with a
// 1pt-wide round-capped/joined pen.
void paintPaper(QPainter& painter, const PaperGeometry& geom, double paneWidthPt, double contentHeightPt,
                 const std::vector<AlpsPlotterMechanism::FlatPoint>& points, std::int32_t penYLower) {
    painter.fillRect(QRectF(0, 0, paneWidthPt, contentHeightPt), Qt::white);
    if (points.empty()) return;

    QPainterPath paths[4];
    bool hasPoints[4] = {false, false, false, false};
    for (const auto& pt : points) {
        const int idx = pt.color % 4;
        const QPointF p = geom.canvasPoint(pt.x, pt.y, penYLower, paneWidthPt);
        if (pt.newStroke || !hasPoints[idx]) {
            paths[idx].moveTo(p);
        } else {
            paths[idx].lineTo(p);
        }
        hasPoints[idx] = true;
    }
    for (int i = 0; i < 4; ++i) {
        if (!hasPoints[i]) continue;
        QPen pen(kPenColors[i], 1.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.drawPath(paths[i]);
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
        const auto [lower, upper] = m_owner->penYRange();
        const double h = m_owner->m_geometry.contentHeight(0, lower, upper, w);
        return QSize(w, static_cast<int>(std::ceil(h)));
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        paintPaper(painter, m_owner->m_geometry, width(), height(), m_owner->m_points, m_owner->penYRange().first);
    }

private:
    PlotterPaperWidget* m_owner;
};

PlotterPaperWidget::PlotterPaperWidget(MachineController* controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Header -- matches DebugPanel's "DEBUG LOG" header exactly (same
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
    m_cutButton = new QPushButton(tr("Cut"), m_buttonBar);
    m_cutButton->setToolTip(tr("Copy the paper, then tear it off (clears the plot)."));
    m_cutButton->setFocusPolicy(Qt::NoFocus);
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

    m_header->setText(m_kind == Kind::CE1600P ? tr("PC-1600P") : tr("CE-150"));
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
    const QString buttonStyle =
        QString("QPushButton { border: none; %1 }"
                "QPushButton:hover:!disabled { background-color: %2; }"
                "QPushButton:pressed, QPushButton:hover:pressed { background-color: %3; }"
                "QPushButton:disabled { background-color: %4; color: %5; }")
            .arg(ChromeStyle::pillCore(c.pillBackground, c.pillText),
                 cssRgba(c.pillBackground.lighter(115)), cssRgba(c.pillBackground.darker(130)),
                 cssRgba(c.pillBackgroundOff), cssRgba(c.pillText));
    m_copyButton->setStyleSheet(buttonStyle);
    m_cutButton->setStyleSheet(buttonStyle);
}

void PlotterPaperWidget::setKind(Kind kind) {
    m_kind = kind;
    m_geometry = kind == Kind::CE1600P ? PaperGeometry::ce1600p() : PaperGeometry::ce150();
    m_points.clear();
    m_lastRevision = UINT64_MAX;
    m_plotArea->updateGeometry();
    m_plotArea->update();
    applyChrome();
    updateButtonsEnabled();
}

std::pair<std::int32_t, std::int32_t> PlotterPaperWidget::penYRange() const {
    if (m_points.empty()) return {0, 0};
    const auto [lo, hi] = std::minmax_element(m_points.begin(), m_points.end(),
                                               [](const auto& a, const auto& b) { return a.y < b.y; });
    return {lo->y, hi->y};
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
    m_points = m_kind == Kind::CE1600P ? m_controller->ce1600pPlotPoints() : m_controller->ce150PlotPoints();
    m_plotArea->updateGeometry();
    m_plotArea->update();
    updateButtonsEnabled();
    if (wasNearBottom) scrollToBottom();
}

void PlotterPaperWidget::copyToClipboard() {
    if (m_points.empty()) return;

    const double paneWidthPt = m_geometry.physicalPaneWidthPt();
    const auto [lower, upper] = penYRange();
    const double contentHeightPt = m_geometry.contentHeight(0, lower, upper, paneWidthPt);
    const double longestDimPt = std::max(paneWidthPt, contentHeightPt);
    const double effectiveScale = std::min(kDpiScale, longestDimPt > 0 ? kMaxTextureDimPx / longestDimPt : kDpiScale);

    QImage image(std::max(1, qRound(paneWidthPt * effectiveScale)),
                 std::max(1, qRound(contentHeightPt * effectiveScale)),
                 QImage::Format_ARGB32);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(effectiveScale, effectiveScale);
    paintPaper(painter, m_geometry, paneWidthPt, contentHeightPt, m_points, lower);
    painter.end();

    // Embed true DPI so a direct file save (or a non-macOS clipboard path)
    // carries physical size too, even when effectiveScale backed off from
    // 600 DPI to respect the texture cap (long rolls).
    const int dotsPerMeter = qRound(effectiveScale * 72.0 / 0.0254);
    image.setDotsPerMeterX(dotsPerMeter);
    image.setDotsPerMeterY(dotsPerMeter);

#ifdef Q_OS_MACOS
    // Qt's cross-platform QClipboard::setImage() ignores both
    // devicePixelRatio and dotsPerMeter when converting to NSImage on
    // macOS (confirmed empirically) -- go straight to Cocoa instead, so
    // Preview/printing see the correct physical size.
    if (macSetClipboardImage(image, paneWidthPt, contentHeightPt)) return;
#endif
    QGuiApplication::clipboard()->setImage(image);
}

void PlotterPaperWidget::cutPaper() {
    copyToClipboard();
    if (m_kind == Kind::CE1600P) m_controller->clearCE1600PPaper();
    else m_controller->clearCE150Paper();
    m_points.clear();
    m_lastRevision = UINT64_MAX;
    m_plotArea->updateGeometry();
    m_plotArea->update();
    updateButtonsEnabled();
}
