#include "LcdWidget.hpp"

#include <QPainter>
#include <QFont>
#include <QFontDatabase>
#include <algorithm>

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

// ── PC-1500/1500A layout constants -- hand-tuned to match the real
// device's indicator-bar/dot-matrix proportions ───────────────────────────
constexpr double kP15IndicatorBarFraction = 18.0 / 42.0;
constexpr double kP15IndicatorFontHeightFraction = 0.277;
constexpr double kP15IndicatorTopInsetFraction = 0.01;
constexpr double kP15ContentScale = 24.52 / 31.1;
constexpr double kP15MatrixTopGapFraction = 0.2;

constexpr double kP15BusyX = 0.0053;
constexpr double kP15ShiftX = 0.0998;
constexpr double kP15JapX = 0.1943;
constexpr double kP15SmallX = 0.2572;
constexpr double kP15AngleX = 0.3579;
constexpr double kP15RunX = 0.5090;
constexpr double kP15ProX = 0.6034;
constexpr double kP15ReserveX = 0.6664;
constexpr double kP15DefX = 0.7922;
constexpr double kP15RomanIX = 0.8740;
constexpr double kP15RomanIIX = 0.8930;
constexpr double kP15RomanIIIX = 0.9245;
constexpr double kP15BatteryX = 0.9874;

// ── PC-1600 layout constants -- hand-tuned; fractions are of the panel's
// own width/height directly. ──────────────────────────────────────────────
constexpr double kP16IndicatorRowTop = 0.03;
constexpr double kP16LabelFontHeightFraction = 26.0 / 337.0; // labelFontSize / canvasSize.height
constexpr double kP16RowHeightFraction = kP16LabelFontHeightFraction * 1.15;
constexpr double kP16MatrixLeft = 0.05;
constexpr double kP16MatrixRight = 0.955;
constexpr double kP16MatrixTop = 0.18;
constexpr double kP16MatrixBottom = 1.00;
constexpr double kP16RowLeft = 0.025;
constexpr double kP16RowRight = 0.9205;

struct P16Item { const char* name; double rawX; };
// rawStartX, measured from source-material/Display.png -- fixed reference
// data, remapped below into [kP16RowLeft, kP16RowRight] exactly like
// Layout.startX(for:) does.
constexpr P16Item kP16Items[] = {
    {"busy", 0.04837}, {"shift", 0.10587}, {"s", 0.17526}, {"kbii", 0.19389},
    {"small", 0.31047}, {"degrad", 0.39611}, {"run", 0.50436}, {"pro", 0.54401},
    {"reserve", 0.60984}, {"def", 0.72086}, {"roman", 0.76209}, {"ctrl", 0.81562},
    {"batt", 0.87192},
};
constexpr double kP16MeasuredLeft = 0.04837;  // kP16Items[0] (busy)
constexpr double kP16MeasuredRight = 0.87192; // kP16Items[last] (batt)

double p16StartX(double rawX) {
    const double t = (rawX - kP16MeasuredLeft) / (kP16MeasuredRight - kP16MeasuredLeft);
    return kP16RowLeft + t * (kP16RowRight - kP16RowLeft);
}

const QColor kP15Background(150, 158, 142);
const QColor kP16Background(156, 168, 149);
const QColor kInk(0x20, 0x28, 0x18);

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
    if (flag("JAPANESE")) drawLabel(kP15JapX, "JPN");
    if (flag("SMALL")) drawLabel(kP15SmallX, "SMALL");
    if (flag("RUN")) drawLabel(kP15RunX, "RUN");
    if (flag("PRO")) drawLabel(kP15ProX, "PRO");
    if (flag("RESERVE")) drawLabel(kP15ReserveX, "RESERVE");
    if (flag("DEF")) drawLabel(kP15DefX, "DEF");
    if (flag("ROMAN_I")) drawLabel(kP15RomanIX, "I");
    if (flag("ROMAN_II")) drawLabel(kP15RomanIIX, "II");
    if (flag("ROMAN_III")) drawLabel(kP15RomanIIIX, "III");

    // DE/G/RAD concatenate into one shared "DEGRAD" template span.
    QString angle;
    if (flag("DE")) angle += "DE";
    if (flag("G")) angle += "G";
    if (flag("RAD")) angle += "RAD";
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

    const auto drawLabel = [&](const char* itemName, const QString& text) {
        painter.setFont(font);
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
    if (flag("KBII")) drawLabel("kbii", QString::fromUtf8("\xE3\x82\xAB\xE3\x83\x8A")); // "カナ"
    if (flag("SMALL")) drawLabel("small", "SMALL");
    if (flag("RUN")) drawLabel("run", "RUN");
    if (flag("PRO")) drawLabel("pro", "PRO");
    if (flag("RESERVE")) drawLabel("reserve", "RESERVE");
    if (flag("DEF")) drawLabel("def", "DEF");
    if (flag("CTRL")) drawLabel("ctrl", "CTRL");
    if (flag("BATT")) drawInvertedLabel("batt", "BATT");

    // DEGRAD: whichever of DEG/RAD/GRAD is set, first match wins.
    if (flag("DEG")) drawLabel("degrad", "DEG");
    else if (flag("RAD")) drawLabel("degrad", "RAD");
    else if (flag("GRAD")) drawLabel("degrad", "GRAD");

    // I/II/III: one shared position, first match wins.
    if (flag("I")) drawLabel("roman", "I");
    else if (flag("II")) drawLabel("roman", "II");
    else if (flag("III")) drawLabel("roman", "III");

    const QRectF matrixArea(kP16MatrixLeft * totalWidth, kP16MatrixTop * totalHeight,
                             (kP16MatrixRight - kP16MatrixLeft) * totalWidth,
                             (kP16MatrixBottom - kP16MatrixTop) * totalHeight);
    paintDotMatrix(painter, matrixArea, kInk);
}
