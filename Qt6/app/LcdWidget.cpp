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

bool LcdWidget::flag(const char* name) const {
    for (const auto& [label, on] : m_frame.statusSymbols) {
        if (label == name) return on;
    }
    return false;
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

    const auto drawLabel = [&](double xFraction, const QString& text) {
        const double x = interior.left() + xFraction * totalWidth;
        const double y = interior.top() + topInset;
        painter.drawText(QRectF(x, y, totalWidth, fontSize * 1.4), Qt::AlignLeft | Qt::AlignTop, text);
    };

    if (flag("BUSY")) drawLabel(kP15BusyX, "BUSY");
    if (flag("SHIFT")) drawLabel(kP15ShiftX, "SHIFT");
    if (flag("KANA")) drawLabel(kP15JapX, "JPN");
    if (flag("SMALL")) drawLabel(kP15SmallX, "SMALL");
    if (flag("RUN")) drawLabel(kP15RunX, "RUN");
    if (flag("PRO")) drawLabel(kP15ProX, "PRO");
    if (flag("RESERVE")) drawLabel(kP15ReserveX, "RESERVE");
    if (flag("DEF")) drawLabel(kP15DefX, "DEF");
    if (flag("I")) drawLabel(kP15RomanIX, "I");
    if (flag("II")) drawLabel(kP15RomanIIX, "II");
    if (flag("III")) drawLabel(kP15RomanIIIX, "III");

    // DE/G/RAD concatenate into one shared "DEGRAD" template span.
    const QString angle = QString::fromStdString(lcdAngleLegend(flag("DE"), flag("G"), flag("RAD")));
    if (!angle.isEmpty()) drawLabel(kP15AngleX, angle);

    // Battery indicator: always on, no data bit backs it.
    {
        const double d = fontSize * 0.4;
        const double x = interior.left() + kP15BatteryX * totalWidth;
        const double y = interior.top() + topInset;
        painter.setPen(Qt::NoPen);
        painter.setBrush(kInk);
        painter.drawEllipse(QRectF(x, y, d, d));
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

    const auto xFor = [&](const char* name) -> double {
        for (const auto& item : kP16Items) {
            if (std::string_view(item.name) == name) return p16StartX(item.rawX) * totalWidth;
        }
        return 0.0;
    };

    const auto drawLabel = [&](const char* itemName, const QString& text, double scale = 1.0) {
        QFont labelFont = font;
        if (scale != 1.0) labelFont.setPixelSize(std::max(1, static_cast<int>(fontSize * scale)));
        painter.setFont(labelFont);
        painter.setPen(kInk);
        const double x = xFor(itemName);
        const QRectF box(x, rowTop, totalWidth - x, rowHeight);
        painter.drawText(box, Qt::AlignLeft | Qt::AlignVCenter, text);
    };

    // "Inverted" labels (S, BATT): filled rounded box in ink, text in the
    // background color.
    const auto drawInvertedLabel = [&](const char* itemName, const QString& text) {
        painter.setFont(font);
        const QFontMetrics fm(font);
        const double x = xFor(itemName);
        const QRectF textRect = fm.boundingRect(text);
        const QRectF box(x - 2, rowTop + (rowHeight - textRect.height()) / 2 - 2,
                          textRect.width() + 4, textRect.height() + 4);
        painter.setPen(Qt::NoPen);
        painter.setBrush(kInk);
        painter.drawRoundedRect(box, 2, 2);
        painter.setPen(kP16Background);
        painter.drawText(QRectF(x, rowTop, totalWidth - x, rowHeight), Qt::AlignLeft | Qt::AlignVCenter, text);
    };

    if (flag("BUSY")) drawLabel("busy", "BUSY");
    if (flag("SHIFT")) drawLabel("shift", "SHIFT");
    if (flag("S")) drawInvertedLabel("s", "S");
    // The romaji->kana caption shares the window after S and is printed
    // small on the glass. The "kana" position is an estimate (the other
    // positions are measured) that keeps both parts clear of SMALL.
    if (flag("ROMAJI")) drawLabel("romaji", QString::fromUtf8("ローマ字"), 0.75);
    if (flag("KANA")) drawLabel("kana", QString::fromUtf8("→カナ"), 0.75);
    if (flag("SMALL")) drawLabel("small", "SMALL");
    if (flag("RUN")) drawLabel("run", "RUN");
    if (flag("PRO")) drawLabel("pro", "PRO");
    if (flag("RESERVE")) drawLabel("reserve", "RESERVE");
    if (flag("DEF")) drawLabel("def", "DEF");
    if (flag("CTRL")) drawLabel("ctrl", "CTRL");
    if (flag("BATT")) drawInvertedLabel("batt", "BATT");

    // DEGRAD: the DE/G/RAD segments read as one word (DE+G = DEG,
    // G+RAD = GRAD), as on the PC-1500.
    const QString angle = QString::fromStdString(lcdAngleLegend(flag("DE"), flag("G"), flag("RAD")));
    if (!angle.isEmpty()) drawLabel("degrad", angle);

    // I/II/III: one shared position, first match wins.
    if (flag("I")) drawLabel("roman", "I");
    else if (flag("II")) drawLabel("roman", "II");
    else if (flag("III")) drawLabel("roman", "III");

    const QRectF matrixArea(kP16MatrixLeft * totalWidth, kP16MatrixTop * totalHeight,
                             (kP16MatrixRight - kP16MatrixLeft) * totalWidth,
                             (kP16MatrixBottom - kP16MatrixTop) * totalHeight);
    paintDotMatrix(painter, matrixArea, kInk);
}
