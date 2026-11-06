#include "LcdWidget.hpp"

#include <QPainter>
#include <QFont>
#include <QFontDatabase>
#include <QMouseEvent>
#include <algorithm>

#include "Shared/LcdLayout.hpp"

namespace {

// The platform's own preferred fixed-width font (Consolas on Windows,
// Menlo/SF Mono on macOS, the desktop's monospace on Linux) rather than a
// hardcoded family name -- "Menlo" doesn't exist outside macOS and was
// silently falling back to a much uglier generic monospace (Courier New on
// Windows). QFontDatabase::systemFont() re-runs font-matching on every
// call, so the family is resolved once here and reused -- paintPC1500/1600
// only need to override the size/weight, which are cheap, at 60Hz.
const QFont& fixedWidthFont() {
    static const QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    return font;
}

// Layout constants and colours: Shared/LcdLayout.hpp.
using namespace shared::lcd;

QColor toQColor(shared::Rgb c) { return QColor(c.r, c.g, c.b); }
const QColor kP15Background = toQColor(kP15BackgroundRgb);
const QColor kP16Background = toQColor(kP16BackgroundRgb);
const QColor kInk = toQColor(kInkRgb);

} // namespace

LcdWidget::LcdWidget(QWidget* parent) : QWidget(parent) {
    setAutoFillBackground(false);
}

void LcdWidget::setModel(Model model) {
    m_model = model;
}

void LcdWidget::setFrame(const DisplayFrame& frame) {
    m_frame = frame;
}

void LcdWidget::mousePressEvent(QMouseEvent* event) {
    emit turboRequested(true);
    event->accept();
}

void LcdWidget::mouseReleaseEvent(QMouseEvent* event) {
    emit turboRequested(false);
    event->accept();
}

void LcdWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    // Antialiased fills -- dots drawn hard-edged look blockier/more
    // aliased at the LCD's small dot pitch.
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (m_model == Model::PC1600) {
        paintPC1600(painter);
    } else {
        paintPC1500(painter);
    }
}

void LcdWidget::paintDotMatrix(QPainter& painter, const QRectF& area, const QColor& ink) {
    if (m_frame.cols <= 0 || m_frame.rows <= 0) return;
    if (!m_frame.poweredOn) return;

    const double dotSpacing = area.width() / m_frame.cols;
    const double rowSpacing = area.height() / m_frame.rows;
    const double dotSize = std::min(dotSpacing, rowSpacing) * 0.9;

    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);

    for (int row = 0; row < m_frame.rows; ++row) {
        for (int col = 0; col < m_frame.cols; ++col) {
            if (!m_frame.pixels[static_cast<std::size_t>(row) * m_frame.cols + col]) continue;
            const double cx = area.left() + (col + 0.5) * dotSpacing;
            const double cy = area.top() + (row + 0.5) * rowSpacing;
            painter.drawRect(QRectF(cx - dotSize / 2, cy - dotSize / 2, dotSize, dotSize));
        }
    }
}

void LcdWidget::paintPC1500(QPainter& painter) {
    // 4px horizontal padding on the whole panel.
    const QRectF interior = rect().adjusted(4, 0, -4, 0);
    painter.fillRect(rect(), kP15Background);
    if (!m_frame.poweredOn) return;

    const double totalHeight = interior.height();
    const double totalWidth = interior.width();
    const double effectiveHeight = totalHeight * kP15ContentScale;
    const double barHeight = effectiveHeight * kP15IndicatorBarFraction / (1 + kP15IndicatorBarFraction);
    const double canvasHeight = effectiveHeight - barHeight;
    const double fontSize = effectiveHeight * kP15IndicatorFontHeightFraction;
    const double topInset = effectiveHeight * kP15IndicatorTopInsetFraction;
    const double matrixTopGap = effectiveHeight * kP15MatrixTopGapFraction;

    QFont font = fixedWidthFont();
    font.setWeight(QFont::DemiBold);
    font.setPixelSize(std::max(1, static_cast<int>(fontSize)));
    painter.setFont(font);
    painter.setPen(kInk);

    const double y = interior.top() + topInset;
    for (const Indicator& ind : indicators(m_model, m_frame)) {
        const double x = interior.left() + ind.x * totalWidth;
        if (ind.style == Indicator::Style::Dot) {
            const double d = fontSize * 0.4;
            painter.setPen(Qt::NoPen);
            painter.setBrush(kInk);
            painter.drawEllipse(QRectF(x, y, d, d));
            continue;
        }
        painter.setPen(kInk);
        painter.drawText(QRectF(x, y, totalWidth, fontSize * 1.4), Qt::AlignLeft | Qt::AlignTop,
                         QString::fromStdString(ind.text));
    }

    const QRectF matrixArea(interior.left(), interior.top() + barHeight + matrixTopGap, totalWidth, canvasHeight);
    paintDotMatrix(painter, matrixArea, kInk);
}

void LcdWidget::paintPC1600(QPainter& painter) {
    painter.fillRect(rect(), kP16Background);
    if (!m_frame.poweredOn) return;

    const double totalWidth = width();
    const double totalHeight = height();
    const double fontSize = totalHeight * kP16LabelFontHeightFraction;
    const double rowHeight = totalHeight * kP16RowHeightFraction;
    const double rowTop = totalHeight * kP16IndicatorRowTop;

    QFont font = fixedWidthFont();
    font.setWeight(QFont::Medium);
    font.setPixelSize(std::max(1, static_cast<int>(fontSize)));

    for (const Indicator& ind : indicators(m_model, m_frame)) {
        const double x = ind.x * totalWidth;
        const QString text = QString::fromStdString(ind.text);
        QFont labelFont = font;
        if (ind.scale != 1.0) labelFont.setPixelSize(std::max(1, static_cast<int>(fontSize * ind.scale)));
        painter.setFont(labelFont);
        const QRectF row(x, rowTop, totalWidth - x, rowHeight);
        if (ind.style == Indicator::Style::Inverted) {
            // A filled rounded box in ink, the text in the background colour.
            const QRectF textRect = QFontMetrics(labelFont).boundingRect(text);
            const QRectF box(x - 2, rowTop + (rowHeight - textRect.height()) / 2 - 2, textRect.width() + 4,
                             textRect.height() + 4);
            painter.setPen(Qt::NoPen);
            painter.setBrush(kInk);
            painter.drawRoundedRect(box, 2, 2);
            painter.setPen(kP16Background);
        } else {
            painter.setPen(kInk);
        }
        painter.drawText(row, Qt::AlignLeft | Qt::AlignVCenter, text);
    }

    const QRectF matrixArea(kP16MatrixLeft * totalWidth, kP16MatrixTop * totalHeight,
                             (kP16MatrixRight - kP16MatrixLeft) * totalWidth,
                             (kP16MatrixBottom - kP16MatrixTop) * totalHeight);
    paintDotMatrix(painter, matrixArea, kInk);
}
