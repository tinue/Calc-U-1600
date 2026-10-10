#include "DebugPanel.hpp"

#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStyle>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>

#include "AppPaths.hpp"
#include "AppSettings.hpp"
#include "ChromeColors.hpp"
#include "Connector/BatteryCardInstance.hpp"
#include "Debug/Inspect/HexDump.hpp"
#include "MachineController.hpp"
#include "MemoryModuleManager.hpp"

namespace {

using inspect::fmt;

std::string joinLines(const std::vector<std::string>& lines) {
    std::string joined;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) joined += '\n';
        joined += lines[i];
    }
    return joined;
}

}  // namespace

DebugPanel::DebugPanel(MachineController* controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ── Header ───────────────────────────────────────────────────────
    m_header = new QLabel(tr("INSPECTOR"), this);
    m_header->setAlignment(Qt::AlignCenter);
    QFont headerFont = m_header->font();
    headerFont.setBold(true);
    headerFont.setPointSize(headerFont.pointSize() - 1);
    m_header->setFont(headerFont);
    outer->addWidget(m_header);

    // ── Output area ──────────────────────────────────────────────────
    m_output = new QPlainTextEdit(this);
    m_output->setReadOnly(true);
    m_output->setLineWrapMode(QPlainTextEdit::NoWrap);
    // Tables don't wrap; a table wider than the panel needs a scroll bar the
    // user can see (macOS hides overlay scroll bars until one scrolls).
    m_output->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    // Never take keyboard focus from MainWindow (which owns physical-
    // keyboard typing for the calculator) -- same convention as
    // ControlBar's widgets.
    m_output->setFocusPolicy(Qt::NoFocus);
    connect(m_output, &QPlainTextEdit::copyAvailable, this, &DebugPanel::outputSelectionChanged);
    QFont monoFont("Menlo");
    monoFont.setStyleHint(QFont::Monospace);
    monoFont.setPointSize(11);
    m_output->setFont(monoFont);
    m_output->setMinimumHeight(120);
    outer->addWidget(m_output, /*stretch=*/1);

    // ── Button bar ───────────────────────────────────────────────────
    m_buttonBar = new QWidget(this);
    auto* buttonBarLayout = new QVBoxLayout(m_buttonBar);
    buttonBarLayout->setContentsMargins(10, 8, 10, 8);
    buttonBarLayout->setSpacing(8);

    auto* row1 = new QHBoxLayout();
    row1->setSpacing(8);
    m_pointersButton = new QPushButton(tr("Pointers"), m_buttonBar);
    m_memoryButton = new QPushButton(tr("Memory"), m_buttonBar);
    m_dumpButton = new QPushButton(tr("Dump"), m_buttonBar);
    m_cardButton = new QPushButton(tr("Card YAML"), m_buttonBar);
    m_cardButton->setToolTip(tr("The module in each slot, as initial-content: YAML for a card file"));
    m_clearButton = new QPushButton(m_buttonBar);
    m_clearButton->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    m_clearButton->setToolTip(tr("Clear"));
    m_clearButton->setEnabled(false);
    m_pointersButton->setObjectName(QStringLiteral("debugpanel.pointers"));
    m_memoryButton->setObjectName(QStringLiteral("debugpanel.memory"));
    m_dumpButton->setObjectName(QStringLiteral("debugpanel.dump"));
    m_cardButton->setObjectName(QStringLiteral("debugpanel.card"));
    m_clearButton->setObjectName(QStringLiteral("debugpanel.clear"));
    for (auto* b : {m_pointersButton, m_memoryButton, m_dumpButton, m_cardButton, m_clearButton}) {
        b->setFocusPolicy(Qt::NoFocus);
    }
    // The menus are filled each time they open: what they offer depends on
    // the model and on what is plugged in (inspect::MenuEntry).
    m_memoryButton->setMenu(new QMenu(m_memoryButton));
    m_dumpButton->setMenu(new QMenu(m_dumpButton));
    connect(m_memoryButton->menu(), &QMenu::aboutToShow, this, [this] { fillMenu(m_memoryButton->menu(), false); });
    connect(m_dumpButton->menu(), &QMenu::aboutToShow, this, [this] { fillMenu(m_dumpButton->menu(), true); });
    row1->addWidget(m_pointersButton);
    row1->addWidget(m_memoryButton);
    row1->addWidget(m_dumpButton);
    row1->addStretch(1);
    row1->addWidget(m_cardButton);
    row1->addWidget(m_clearButton);
    buttonBarLayout->addLayout(row1);

    auto* row2 = new QHBoxLayout();
    row2->setSpacing(8);
    m_traceButton = new QToolButton(m_buttonBar);
    m_traceButton->setObjectName(QStringLiteral("debugpanel.trace"));
    m_traceButton->setText(tr(" TRACE"));
    m_traceButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_traceButton->setCheckable(false);
    m_traceButton->setAutoRaise(true);
    m_traceButton->setFocusPolicy(Qt::NoFocus);
    m_logButton = new QToolButton(m_buttonBar);
    m_logButton->setText(tr(" LOG"));
    m_logButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_logButton->setAutoRaise(true);
    m_logButton->setFocusPolicy(Qt::NoFocus);
    row2->addWidget(m_traceButton);
    row2->addWidget(m_logButton);
    row2->addStretch(1);
    buttonBarLayout->addLayout(row2);

    outer->addWidget(m_buttonBar);

    connect(m_pointersButton, &QPushButton::clicked, this, [this] { showView(inspect::View::Pointers); });
    connect(m_cardButton, &QPushButton::clicked, this, &DebugPanel::debugDumpModuleCardAsYaml);
    connect(m_clearButton, &QPushButton::clicked, this, &DebugPanel::clearDebug);
    connect(m_traceButton, &QToolButton::clicked, this, [this] { setTraceEnabled(!m_controller->traceActive()); });
    connect(m_logButton, &QToolButton::clicked, this, &DebugPanel::toggleDebugLevel);

    applyChrome();
}

QString DebugPanel::selectedOutputText() const {
    // QTextCursor uses U+2029 as the paragraph separator.
    return m_output->textCursor().selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
}

DebugPanel::~DebugPanel() {
    m_controller->endTrace();
}

bool DebugPanel::isDarkMode() const {
    return ChromeColors::isDarkMode(this);
}

void DebugPanel::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
        applyChrome();
    }
}

void DebugPanel::applyChrome() {
    const ChromeColors c = ChromeColors::forWidget(this);
    m_pillOnColor = c.pillBackground;
    m_pillOffColor = c.pillBackgroundOff;
    m_pillTextColor = c.pillText;

    m_header->setStyleSheet(ChromeStyle::header(c));
    m_output->setStyleSheet(
        QString("QPlainTextEdit { background-color: %1; color: %2;"
                " border: 1px solid %3; border-radius: 2px; }")
            .arg(cssRgba(c.outputBackground), cssRgba(c.outputText), cssRgba(c.outputBorder)));
    m_buttonBar->setStyleSheet(ChromeStyle::buttonBar(c));

    updateTraceButtonAppearance();
    updateLogButtonAppearance();
}

QIcon DebugPanel::dotIcon(const QColor& color) const {
    QPixmap pixmap(8, 8);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(0, 0, 8, 8);
    return QIcon(pixmap);
}

// ── Ring buffer / paging ──────────────────────────────────────────────

void DebugPanel::ringWrite(const std::string& line) {
    m_ringBuf[m_ringHead % kRingBufCap] = line;
    ++m_ringHead;
}

void DebugPanel::ringWriteAll(const std::vector<std::string>& lines) {
    for (const auto& line : lines) ringWrite(line);
    refreshDebugDisplay();
}

void DebugPanel::refreshDebugDisplay() {
    const int head = m_ringHead;
    const int newSinceLast = head - m_debugLastDisplayHead;
    m_debugLastDisplayHead = head;

    const int totalAvailable = std::min(head, kRingBufCap);
    if (totalAvailable <= 0) {
        m_output->setPlainText(QString());
        m_clearButton->setEnabled(false);
        return;
    }

    const int linesToShow = newSinceLast > kDisplayLinesPerPage
        ? std::min(kDisplayLinesPerPage, totalAvailable)
        : std::min(kDisplayLinesPerPage * kDisplayPageCap, totalAvailable);
    const int startPos = head - linesToShow;

    QString text;
    for (int i = 0; i < linesToShow; ++i) {
        if (i > 0) text += '\n';
        text += QString::fromStdString(m_ringBuf[(startPos + i) % kRingBufCap]);
    }
    m_output->setPlainText(text);
    // To the last line's start, not its end: a wide table or dump line would
    // otherwise scroll the view sideways to its right edge.
    QTextCursor cursor = m_output->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.movePosition(QTextCursor::StartOfBlock);
    m_output->setTextCursor(cursor);
    m_output->horizontalScrollBar()->setValue(0);
    m_clearButton->setEnabled(true);
}

void DebugPanel::clearDebug() {
    m_ringHead = 0;
    m_debugLastDisplayHead = 0;
    m_output->setPlainText(QString());
    m_clearButton->setEnabled(false);
}

// ── TRACE ─────────────────────────────────────────────────────────────

void DebugPanel::setTraceEnabled(bool enabled) {
    if (m_controller->traceActive() == enabled) return;

    if (enabled) {
        const QString dir = AppSettings::traceDirOverride().isEmpty() ? AppPaths::instanceDir()
                                                                        : AppSettings::traceDirOverride();
        const QString path = dir + "/TRACE.bin";
        if (!m_controller->beginTrace(path)) {
            ringWriteAll({fmt("TRACE: couldn't start a capture to %s.", path.toStdString().c_str())});
            updateTraceButtonAppearance();
            return;
        }
        const int maxMB = AppSettings::traceMaxFileSizeMB();
        m_traceMaxBytes = static_cast<std::uint64_t>(maxMB > 0 ? maxMB : 50) * 1'000'000;
    } else {
        m_controller->endTrace(); // drains the rest, writes SESSION_END, closes the file
    }
    updateTraceButtonAppearance();
}

void DebugPanel::onFrameTick() {
    if (!m_traceShownActive) return;
    if (!m_controller->traceActive()) {
        // Nothing here stopped it: a machine rebuild did.
        ringWriteAll({"TRACE stopped: the machine was rebuilt (model, ROM, module or preset change). "
                      "Re-enable TRACE to start a new session."});
        updateTraceButtonAppearance();
        return;
    }
    if (m_controller->traceBytes() < m_traceMaxBytes) return;
    ringWriteAll({fmt("TRACE stopped: reached the %llu MB size limit. Re-enable TRACE to start a new session.",
                       static_cast<unsigned long long>(m_traceMaxBytes / 1'000'000))});
    setTraceEnabled(false);
}

void DebugPanel::updateTraceButtonAppearance() {
    const bool available = m_controller->hasLiveMachine();
    m_traceShownActive = m_controller->traceActive();
    const QColor dot = !available ? QColor(255, 0, 0, 128)
                                   : (m_traceShownActive ? QColor(255, 165, 0) : QColor(128, 128, 128, 128));
    m_traceButton->setIcon(dotIcon(dot));
    m_traceButton->setEnabled(available);
    const QColor bg = available ? m_pillOnColor : m_pillOffColor;
    m_traceButton->setStyleSheet(
        QString("QToolButton { %1 }"
                " QToolButton:disabled { color: %2; }")
            .arg(ChromeStyle::pillCore(bg, m_pillTextColor), cssRgba(m_pillTextColor)));
}

// ── LOG (placeholder) ──────────────────────────────────────────────────

void DebugPanel::toggleDebugLevel() {
    m_debugLevel = static_cast<DebugLevel>((static_cast<int>(m_debugLevel) + 1) % 3);
    updateLogButtonAppearance();
}

void DebugPanel::updateLogButtonAppearance() {
    QColor dot;
    switch (m_debugLevel) {
        case DebugLevel::Off:   dot = QColor(128, 128, 128, 128); break;
        case DebugLevel::Info:  dot = QColor(255, 165, 0); break;
        case DebugLevel::Debug: dot = QColor(255, 0, 0); break;
    }
    m_logButton->setIcon(dotIcon(dot));
    m_logButton->setStyleSheet(QString("QToolButton { %1 }").arg(ChromeStyle::pillCore(m_pillOnColor, m_pillTextColor)));
}

// ── Inspector views ────────────────────────────────────────────────────

void DebugPanel::fillMenu(QMenu* menu, bool dumps) {
    menu->clear();
    const bool pc1500 = m_controller->currentModel() != Model::PC1600;
    for (const inspect::MenuEntry& e : m_controller->inspectorMenu(dumps)) {
        QAction* a = menu->addAction(QString::fromUtf8(inspect::viewTitle(e.view, pc1500)));
        a->setEnabled(e.enabled);
        const inspect::View v = e.view;
        connect(a, &QAction::triggered, this, [this, v] { showView(v); });
    }
}

void DebugPanel::showView(inspect::View view) {
    const std::vector<std::string> lines = m_controller->inspectorView(view);
    // One ring entry, so a long dump isn't cut to the last display page.
    if (!lines.empty()) ringWriteAll({joinLines(lines)});
}

// ── Dump Card YAML ──────────────────────────────────────────────────────

bool DebugPanel::batteryCardImage(int slot, int* bankCount, std::vector<std::uint8_t>* image) {
    *image = m_controller->debugSlotCardImage(slot, bankCount);
    // bankCount <= 0 is an unbanked card (e.g. CE-1600M): one implicit bank,
    // which formatBatteryCardInitialContentBlock() writes without a `bank:` key.
    if (image->empty()) return false;
    return *bankCount <= 0 || image->size() % *bankCount == 0;
}

void DebugPanel::debugDumpModuleCardAsYaml() {
    const bool isPC1600 = m_controller->currentModel() == Model::PC1600;
    const std::vector<int> slotsToDump = isPC1600 ? std::vector<int>{1, 2} : std::vector<int>{1};
    std::vector<std::string> chunks;
    for (int slot : slotsToDump) {
        int bankCount = 0;
        std::vector<std::uint8_t> image;
        if (!batteryCardImage(slot, &bankCount, &image)) continue;
        const auto lines = formatBatteryCardInitialContentBlock(bankCount, image);
        const std::string name = m_moduleManager ? m_moduleManager->selectedModuleName(slot).toStdString() : std::string();
        const std::string label = name.empty() ? "attached module" : name;
        const std::string slotLabel = isPC1600 ? fmt(" (Slot %d)", slot) : "";
        const std::string layout = bankCount > 0
            ? fmt("%d bank(s) x %s", bankCount, inspect::sizeLabel(static_cast<std::uint32_t>(image.size() / bankCount)).c_str())
            : fmt("unbanked, %s", inspect::sizeLabel(static_cast<std::uint32_t>(image.size())).c_str());
        chunks.push_back(fmt("\xE2\x94\x80\xE2\x94\x80 Module card dump: %s%s, %s -- paste under the region's initial-content: \xE2\x94\x80\xE2\x94\x80",
                              label.c_str(), slotLabel.c_str(), layout.c_str()));
        chunks.push_back(joinLines(lines));
    }
    if (chunks.empty()) {
        ringWriteAll({"\xE2\x94\x80\xE2\x94\x80 Module card dump: no card attached, or it has no dumpable RAM \xE2\x94\x80\xE2\x94\x80"});
        return;
    }
    ringWriteAll(chunks);
}
