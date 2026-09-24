#include "DebugPanel.hpp"

#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
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
#include <cstdarg>
#include <cstdio>
#include <set>

#include "AppPaths.hpp"
#include "AppSettings.hpp"
#include "ChromeColors.hpp"
#include "Connector/BatteryCardInstance.hpp"
#include "Debug/BasicPointerTable.hpp"
#include "MachineController.hpp"
#include "MemoryModuleManager.hpp"

namespace {

std::string padRight(const std::string& s, std::size_t width) {
    return s.size() >= width ? s : s + std::string(width - s.size(), ' ');
}

std::string fmt(const char* format, ...) {
    char buf[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return buf;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string joined;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) joined += '\n';
        joined += lines[i];
    }
    return joined;
}

std::string hexRowOf(const std::vector<std::uint8_t>& bytes, std::size_t start, std::size_t end) {
    std::string hex;
    for (std::size_t i = start; i < end; ++i) {
        hex += fmt("%02X ", bytes[i]);
        if (i - start == 7) hex += " ";
    }
    while (!hex.empty() && hex.back() == ' ') hex.pop_back();
    return hex;
}

}  // namespace

DebugPanel::DebugPanel(MachineController* controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ── Header ───────────────────────────────────────────────────────
    m_header = new QLabel(tr("DEBUG LOG"), this);
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
    m_dumpMemButton = new QPushButton(tr("Dump Mem"), m_buttonBar);
    m_dumpCardButton = new QPushButton(tr("Dump Card YAML"), m_buttonBar);
    m_clearButton = new QPushButton(m_buttonBar);
    m_clearButton->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    m_clearButton->setToolTip(tr("Clear"));
    m_clearButton->setEnabled(false);
    for (auto* b : {m_pointersButton, m_dumpMemButton, m_dumpCardButton, m_clearButton}) {
        b->setFocusPolicy(Qt::NoFocus);
    }
    row1->addWidget(m_pointersButton);
    row1->addWidget(m_dumpMemButton);
    row1->addStretch(1);
    row1->addWidget(m_dumpCardButton);
    row1->addWidget(m_clearButton);
    buttonBarLayout->addLayout(row1);

    auto* row2 = new QHBoxLayout();
    row2->setSpacing(8);
    m_traceButton = new QToolButton(m_buttonBar);
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

    connect(m_pointersButton, &QPushButton::clicked, this, &DebugPanel::debugDumpPointers);
    connect(m_dumpMemButton, &QPushButton::clicked, this, &DebugPanel::debugDumpMemory);
    connect(m_dumpCardButton, &QPushButton::clicked, this, &DebugPanel::debugDumpModuleCardAsYaml);
    connect(m_clearButton, &QPushButton::clicked, this, &DebugPanel::clearDebug);
    connect(m_traceButton, &QToolButton::clicked, this, [this] { setTraceEnabled(!m_traceEnabled); });
    connect(m_logButton, &QToolButton::clicked, this, &DebugPanel::toggleDebugLevel);
    connect(m_controller, &MachineController::traceEndedByRebuild, this, &DebugPanel::onTraceEndedByRebuild);

    applyChrome();
}

QString DebugPanel::selectedOutputText() const {
    // QTextCursor uses U+2029 as the paragraph separator.
    return m_output->textCursor().selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
}

DebugPanel::~DebugPanel() {
    if (m_traceEnabled) m_controller->endTrace();
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
    QTextCursor cursor = m_output->textCursor();
    cursor.movePosition(QTextCursor::End);
    m_output->setTextCursor(cursor);
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
    if (m_traceEnabled == enabled) return;
    m_traceEnabled = enabled;

    if (enabled) {
        const QString dir = AppSettings::traceDirOverride().isEmpty() ? AppPaths::instanceDir()
                                                                        : AppSettings::traceDirOverride();
        m_tracePath = dir + "/TRACE.bin";
        if (!m_controller->beginTrace(m_tracePath)) {
            m_traceEnabled = false;
            ringWriteAll({fmt("TRACE: couldn't start a capture to %s.", m_tracePath.toStdString().c_str())});
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

void DebugPanel::onTraceEndedByRebuild() {
    if (!m_traceEnabled) return;
    m_traceEnabled = false;
    ringWriteAll({"TRACE stopped: the machine was rebuilt (model, ROM, module or preset change). "
                  "Re-enable TRACE to start a new session."});
    updateTraceButtonAppearance();
}

void DebugPanel::checkTraceSizeLimit() {
    // Core writes the file; its size on disk (short of stdio's buffer) is
    // the running byte count.
    if (static_cast<std::uint64_t>(QFileInfo(m_tracePath).size()) < m_traceMaxBytes) return;
    ringWriteAll({fmt("TRACE stopped: reached the %llu MB size limit. Re-enable TRACE to start a new session.",
                       static_cast<unsigned long long>(m_traceMaxBytes / 1'000'000))});
    setTraceEnabled(false);
}

void DebugPanel::onFrameTick() {
    if (!m_traceEnabled) return;
    checkTraceSizeLimit();
}

void DebugPanel::updateTraceButtonAppearance() {
    const bool available = m_controller->hasLiveMachine();
    const QColor dot = !available ? QColor(255, 0, 0, 128)
                                   : (m_traceEnabled ? QColor(255, 165, 0) : QColor(128, 128, 128, 128));
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

// ── Pointers dump ──────────────────────────────────────────────────────

void DebugPanel::debugDumpPointers() {
    if (m_controller->currentModel() == Model::PC1600) debugDumpPointersPC1600(); else debugDumpPointersPC1500();
}

void DebugPanel::debugDumpPointersPC1500() {
    std::vector<std::string> lines = {"\xE2\x94\x80\xE2\x94\x80 BASIC Pointers \xE2\x94\x80\xE2\x94\x80"};
    std::uint16_t basPrgEnd = 0;
    std::uint8_t ramEndPage = 0;
    const int nameWidth = CoreDebug::kPC1500BasicPointerMaxNameLength;
    for (int i = 0; i < CoreDebug::kPC1500BasicPointerCount; ++i) {
        const auto& p = CoreDebug::kPC1500BasicPointers[i];
        const std::string name = padRight(p.name, nameWidth);
        std::string value;
        std::string description = p.description;
        if (p.width == CoreDebug::BasicPointerEntry::Width::Byte) {
            const std::uint8_t v = m_controller->debugPeek(p.address);
            if (i == CoreDebug::kPC1500RamEndIndex) ramEndPage = v;
            value = padRight(fmt("$%02X", v), 6);
            if (i == CoreDebug::kPC1500LockIndex) {
                description = (v == 0xFF ? "(unlocked) " : "(locked) ");
                description += (v == 0xFF ? p.description : "Lock register; MODE key is blocked");
            }
        } else {
            const std::uint8_t hi = m_controller->debugPeek(p.address);
            const std::uint8_t lo = m_controller->debugPeek(p.address + 1);
            const std::uint16_t word = (static_cast<std::uint16_t>(hi) << 8) | lo;
            if (i == CoreDebug::kPC1500BasPrgEndIndex) basPrgEnd = word;
            value = padRight(fmt("$%04X", word), 6);
        }
        lines.push_back(fmt("%s $%04X = %s-- %s", name.c_str(), p.address, value.c_str(), description.c_str()));
    }
    const std::uint16_t ramEndAddr = static_cast<std::uint16_t>(ramEndPage) << 8;
    const int mem = static_cast<int>(ramEndAddr) - static_cast<int>(basPrgEnd) - 1;
    lines.push_back(fmt("%s (computed) = %d bytes free  (RAM_END=$%04X - BASPRG_END - 1)",
                         padRight("MEM", nameWidth).c_str(), mem, ramEndAddr));
    ringWriteAll(lines);
}

void DebugPanel::debugDumpPointersPC1600() {
    std::vector<std::string> lines = {"\xE2\x94\x80\xE2\x94\x80 PC-1600 Pointers \xE2\x94\x80\xE2\x94\x80  (SC-7852 view, internal RAM)"};
    const int nameWidth = CoreDebug::kPC1600PointerMaxNameLength;
    std::uint16_t basPrgEnd = 0;
    for (int i = 0; i < CoreDebug::kPC1600PointerCount; ++i) {
        const auto& p = CoreDebug::kPC1600Pointers[i];
        const std::string name = padRight(p.name, nameWidth);
        std::string value;
        if (p.value == CoreDebug::PC1600PointerEntry::Value::Byte) {
            value = fmt("$%02X", m_controller->debugPeek(p.address));
        } else {
            const std::uint8_t b0 = m_controller->debugPeek(p.address);
            const std::uint8_t b1 = m_controller->debugPeek(p.address + 1);
            const std::uint16_t word = p.value == CoreDebug::PC1600PointerEntry::Value::WordBE
                ? (static_cast<std::uint16_t>(b0) << 8) | b1
                : (static_cast<std::uint16_t>(b1) << 8) | b0;
            if (i == CoreDebug::kPC1600BasPrgEndIndex) basPrgEnd = word;
            value = fmt("$%04X (%d)", word, word);
        }
        lines.push_back(fmt("%s $%04X = %s %s", name.c_str(), p.address, padRight(value, 14).c_str(), p.note));
    }
    const int gap = static_cast<int>(CoreDebug::kPC1600WorkAreaBase) - static_cast<int>(basPrgEnd);
    lines.push_back(fmt("work-area base $%04X \xE2\x88\x92 BASPRG_END = %d bytes", CoreDebug::kPC1600WorkAreaBase, gap));
    ringWriteAll(lines);
}

// ── Dump Mem (PC1500) ──────────────────────────────────────────────────

std::string DebugPanel::debugSizeLabel(int bytes) {
    return bytes % 1024 == 0 ? fmt("%dk", bytes / 1024) : fmt("%dB", bytes);
}

std::vector<std::string> DebugPanel::debugDumpMemRegion(const MemRegion& region) {
    static const std::set<std::uint8_t> ignored = {0x00, 0xFF, 0xAA};
    std::vector<std::string> lines;
    if (region.hasLabel) {
        lines.push_back(fmt(".... $%04X-$%04X (%s) %s", region.start, region.end - 1,
                             debugSizeLabel(region.end - region.start).c_str(), region.label.c_str()));
    }
    if (region.suppressDump) return lines;

    const int rowCount = (region.end - region.start + 15) / 16;
    int skipRunStart = -1;
    auto flushSkip = [&](int upTo) {
        if (skipRunStart < 0) return;
        lines.push_back(region.hasLabel ? std::string("...") : fmt(".... $%04X-$%04X", skipRunStart, upTo - 1));
        skipRunStart = -1;
    };

    int rowBase = region.start;
    int rowIndex = 0;
    while (rowBase < region.end) {
        const int rowEnd = std::min(rowBase + 16, region.end);
        std::vector<std::uint8_t> bytes;
        for (int a = rowBase; a < rowEnd; ++a) bytes.push_back(m_controller->debugPeek(static_cast<std::uint16_t>(a)));

        const bool forcePrint = region.hasLabel && (rowIndex == 0 || rowIndex == rowCount - 1);
        bool allIgnored = true;
        for (auto b : bytes) if (!ignored.count(b)) { allIgnored = false; break; }
        if (allIgnored && !forcePrint) {
            if (skipRunStart < 0) skipRunStart = rowBase;
        } else {
            flushSkip(rowBase);
            lines.push_back(fmt("$%04X: %s", static_cast<std::uint16_t>(rowBase), hexRowOf(bytes, 0, bytes.size()).c_str()));
        }
        rowBase = rowEnd;
        ++rowIndex;
    }
    flushSkip(region.end);
    return lines;
}

void DebugPanel::debugDumpMemory() {
    if (m_controller->currentModel() == Model::PC1600) debugDumpMemoryPC1600(); else debugDumpMemoryPC1500();
}

void DebugPanel::debugDumpMemoryPC1500() {
    const std::string slot1Name = m_moduleManager ? m_moduleManager->selectedModuleName(1).toStdString() : std::string();

    auto moduleRegion = [&](int start, int end) -> MemRegion {
        if (slot1Name.empty()) return MemRegion{start, end, "", false};
        if (!m_controller->debugSlotResponds(static_cast<std::uint16_t>(start)))
            return MemRegion{start, end, "Empty", true, true};
        const int bank = m_controller->debugSlotCardBank();
        const std::string label = bank >= 0 ? fmt("%s Bank %d", slot1Name.c_str(), bank) : slot1Name;
        return MemRegion{start, end, label, true};
    };

    std::vector<MemRegion> regions;
    regions.push_back(moduleRegion(0x0000, 0x4000));
    if (m_controller->currentModel() == Model::PC1500A) {
        regions.push_back(MemRegion{0x4000, 0x5800, "Built-in User RAM", true});
        regions.push_back(moduleRegion(0x5800, 0x7000));
    } else {
        regions.push_back(MemRegion{0x4000, 0x4800, "Built-in User RAM", true});
        regions.push_back(moduleRegion(0x4800, 0x7000));
    }
    regions.push_back(MemRegion{0x7000, 0x7800, "Display RAM", true});
    regions.push_back(MemRegion{0x7800, 0x7C00, "System RAM", true});
    regions.push_back(MemRegion{0x7C00, 0x8000, "Machine language area", true});

    std::vector<std::string> body;
    for (const auto& region : regions) {
        auto lines = debugDumpMemRegion(region);
        body.insert(body.end(), lines.begin(), lines.end());
    }

    const std::string header = "\xE2\x94\x80\xE2\x94\x80 RAM Dump $0000-$7FFF (skipping empty areas like 00/FF/AA) \xE2\x94\x80\xE2\x94\x80";
    ringWriteAll({header, body.empty() ? "(every row is 00/FF/AA only)" : joinLines(body)});
}

// ── Dump Card YAML ──────────────────────────────────────────────────────

bool DebugPanel::batteryCardImage(int slot, int* bankCount, std::vector<std::uint8_t>* image) {
    if (m_controller->currentModel() == Model::PC1600) {
        const auto bs = m_controller->debugBankStatePC1600();
        *bankCount = slot == 2 ? bs.slot2CardBankCount : bs.slot1CardBankCount;
        *image = m_controller->debugSlotImagePC1600(slot);
    } else {
        *bankCount = m_controller->debugSlotCardBankCount();
        *image = m_controller->debugSlotCardImage();
    }
    if (*bankCount <= 0 || image->empty() || image->size() % *bankCount != 0) return false;
    return true;
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
        const std::size_t bankSize = image.size() / bankCount;
        const std::string name = m_moduleManager ? m_moduleManager->selectedModuleName(slot).toStdString() : std::string();
        const std::string label = name.empty() ? "attached module" : name;
        const std::string slotLabel = isPC1600 ? fmt(" (Slot %d)", slot) : "";
        chunks.push_back(fmt("\xE2\x94\x80\xE2\x94\x80 Module card dump: %s%s, %d bank(s) x %s -- paste under the region's initial-content: \xE2\x94\x80\xE2\x94\x80",
                              label.c_str(), slotLabel.c_str(), bankCount, debugSizeLabel(static_cast<int>(bankSize)).c_str()));
        chunks.push_back(joinLines(lines));
    }
    if (chunks.empty()) {
        ringWriteAll({"\xE2\x94\x80\xE2\x94\x80 Module card dump: no card attached, or it has no bank concept \xE2\x94\x80\xE2\x94\x80"});
        return;
    }
    ringWriteAll(chunks);
}

// ── Dump Mem (PC1600) ───────────────────────────────────────────────────

namespace {

std::string targetName(DebugPageTarget t, bool ce1600pAttached, const std::string& s1, const std::string& s2) {
    switch (t) {
        case DebugPageTarget::SystemRomLo:   return "system ROM (lower 16 KB)";
        case DebugPageTarget::SystemRomHi:   return "system ROM (upper 16 KB)";
        case DebugPageTarget::Bank3Rom:      return "Bank 3 BASIC ROM";
        case DebugPageTarget::Bank3bRom:     return "Bank 3b hidden BASIC ROM";
        case DebugPageTarget::Bank6Rom:      return "Bank 6 ROM (ROM IV)";
        case DebugPageTarget::PeripheralRom:
            return ce1600pAttached ? "CE-1600P peripheral ROM" : "CE-1600P peripheral ROM (not attached)";
        case DebugPageTarget::InternalRam:   return "internal 16 KB RAM";
        case DebugPageTarget::Slot1:         return "Slot 1 \xE2\x80\x94 " + s1;
        case DebugPageTarget::Slot2:         return "Slot 2 \xE2\x80\x94 " + s2;
        case DebugPageTarget::OpenBus:       return "open bus";
    }
    return "?";
}

std::vector<std::string> hexRowsFrom(const std::vector<std::uint8_t>& bytes, int base) {
    static const std::set<std::uint8_t> ignored = {0x00, 0xFF};
    std::vector<std::string> out;
    int skip = -1;
    auto flush = [&](int upTo) {
        if (skip < 0) return;
        out.push_back(fmt("    .... $%04X\xE2\x80\x93$%04X  (all 00/FF)",
                           static_cast<std::uint16_t>(skip), static_cast<std::uint16_t>(upTo - 1)));
        skip = -1;
    };
    std::size_t off = 0;
    int addr = base;
    while (off < bytes.size()) {
        const std::size_t end = std::min(off + 16, bytes.size());
        bool allIgnored = true;
        for (std::size_t k = off; k < end; ++k) if (!ignored.count(bytes[k])) { allIgnored = false; break; }
        if (allIgnored) {
            if (skip < 0) skip = addr;
        } else {
            flush(addr);
            out.push_back(fmt("    $%04X: %s", static_cast<std::uint16_t>(addr), hexRowOf(bytes, off, end).c_str()));
        }
        off += 16;
        addr += 16;
    }
    flush(addr);
    return out;
}

}  // namespace

void DebugPanel::debugDumpMemoryPC1600() {
    const auto bs = m_controller->debugBankStatePC1600();
    const bool slot1Present = m_controller->slot1Attached();
    const bool slot2Present = m_controller->slot2Attached();

    const std::string s1sel = m_moduleManager ? m_moduleManager->selectedModuleName(1).toStdString() : std::string();
    const std::string s2sel = m_moduleManager ? m_moduleManager->selectedModuleName(2).toStdString() : std::string();
    const std::string s1name = !s1sel.empty() ? s1sel : (slot1Present ? "attached" : "\xE2\x80\x94");
    const std::string s2name = !s2sel.empty() ? s2sel : (slot2Present ? "attached" : "\xE2\x80\x94");
    auto withBank = [](const std::string& name, int cardBank) {
        return cardBank >= 0 ? fmt("%s (bank %d)", name.c_str(), cardBank) : name;
    };
    const std::string s1 = withBank(s1name, bs.slot1CardBank);
    const std::string s2 = withBank(s2name, bs.slot2CardBank);

    std::vector<std::string> lines = {"\xE2\x94\x80\xE2\x94\x80 PC-1600 memory dump \xE2\x94\x80\xE2\x94\x80", ""};

    // View 1: live address map.
    const char* pageLabels[4] = {"A  $0000\xE2\x80\x93$3FFF", "B  $4000\xE2\x80\x93$7FFF", "C  $8000\xE2\x80\x93$BFFF", "D  $C000\xE2\x80\x93$FFFF"};
    const std::uint8_t pageBanks[4] = {bs.pageABank, bs.pageBBank, bs.pageCBank, bs.pageDBank};
    lines.push_back(fmt("\xE2\x94\x80\xE2\x94\x80 address map (live)   31H=$%02X  28H=$%02X  3CH=$%02X \xE2\x94\x80\xE2\x94\x80",
                         bs.port31, bs.port28, bs.port3c));
    for (int i = 0; i < 4; ++i) {
        std::string row = fmt("  %s   bank %d   %s", pageLabels[i], pageBanks[i],
                               targetName(bs.target[i], m_controller->ce1600pAttached(), s1, s2).c_str());
        if (bs.slotmapRedirect[i]) row += fmt("   \xE2\x97\x80 SLOTMAP mode %d redirect", bs.slot2MapMode);
        lines.push_back(row);
    }
    lines.push_back("");

    const std::string pageRowLabels[4] = {"A  $0000", "B  $4000", "C  $8000", "D  $C000"};
    const int rowLabelW = 10;

    const std::string slot1MapDesc = bs.slot1MapRemapped
        ? "1  Slot 1 high 16 KB half (beta) also answers at page-B bank 1"
        : "0  default \xE2\x80\x94 Slot 1 \xE2\x86\x92 page-C banks 0/1";
    std::string slot2MapDesc;
    switch (bs.slot2MapMode) {
        case 1:  slot2MapDesc = "1  Slot 2 low 16 KB also answers at page-C bank 1"; break;
        case 2:  slot2MapDesc = "2  Slot 2 \xE2\x86\x92 page-B bank 1 (4000\xE2\x80\x93" "7FFF) / page-A bank 1 (0000\xE2\x80\x93" "3FFF)"; break;
        default: slot2MapDesc = "0  default \xE2\x80\x94 Slot 2 \xE2\x86\x92 page-C banks 2/3"; break;
    }
    std::string vbankDesc;
    if (slot2Present) {
        vbankDesc = bs.slot2CardBank >= 0
            ? fmt("%d  (module latch; last OUT 28H = %d)", bs.slot2CardBank, bs.port28)
            : fmt("n/a \xE2\x80\x94 module ignores Port 28H, aliases all vbanks (last OUT 28H = %d)", bs.port28);
    } else {
        vbankDesc = fmt("%d  (last OUT 28H; Slot 2 empty)", bs.port28);
    }
    lines.push_back("SLOT1MAP b2 = " + slot1MapDesc);
    lines.push_back("SLOT2MAP    = " + slot2MapDesc);
    lines.push_back("Slot 2 vertical bank = " + vbankDesc);
    lines.push_back("");

    auto slotMapRow = [&](const std::string& label, const std::vector<std::string>& candidates) {
        std::string joined;
        for (std::size_t i = 0; i < candidates.size(); ++i) { if (i) joined += " \xE2\x9A\xA1 "; joined += candidates[i]; }
        return padRight(label, rowLabelW) + (candidates.empty() ? "\xC2\xB7" : joined);
    };
    std::vector<std::string> pageB1Candidates;
    if (bs.slot1MapRemapped) pageB1Candidates.push_back("Slot 1 \xCE\xB2 (high)");
    if (bs.slot2MapMode == 2) pageB1Candidates.push_back("Slot 2 \xCE\xB1 (low)");
    lines.push_back("\xE2\x94\x80\xE2\x94\x80 slot map grid (armed now) \xE2\x94\x80\xE2\x94\x80");
    lines.push_back(slotMapRow("page-A bank1", bs.slot2MapMode == 2 ? std::vector<std::string>{"Slot 2 \xCE\xB2 (high)"} : std::vector<std::string>{}));
    lines.push_back(slotMapRow("page-B bank1", pageB1Candidates));
    lines.push_back(slotMapRow("page-C bank1", bs.slot2MapMode == 1 ? std::vector<std::string>{"Slot 2 \xCE\xB1 (low)"} : std::vector<std::string>{}));
    lines.push_back("");

    // View 3: resource inventory.
    auto resourceCell = [&](int page, int bank) -> std::vector<std::string> {
        if (page == 0 && (bank == 0 || bank == 1)) return {"System ROM lo"};
        if (page == 1 && bank == 0) return {"System ROM hi"};
        if (page == 1 && bank == 3) return {"BASIC ROM (bank 3)", "Hidden BASIC ROM (bank 3b)"};
        if (page == 2 && (bank == 0 || bank == 1)) return slot1Present ? std::vector<std::string>{s1name} : std::vector<std::string>{};
        if (page == 2 && (bank == 2 || bank == 3)) return slot2Present ? std::vector<std::string>{s2name} : std::vector<std::string>{};
        if (page == 2 && bank == 6) return {"Bank 6 ROM"};
        if (page == 3 && bank == 0) return {"Internal RAM"};
        return {};
    };
    const int colW = 18;
    lines.push_back("\xE2\x94\x80\xE2\x94\x80 resource inventory (built-in + plugged-in) \xE2\x94\x80\xE2\x94\x80");
    {
        std::string header = padRight("", rowLabelW);
        for (int b = 0; b <= 6; ++b) header += padRight(fmt("bank %d", b), colW);
        lines.push_back(header);
    }
    for (int page = 0; page < 4; ++page) {
        std::string row = padRight(pageRowLabels[page], rowLabelW);
        int skipCols = 0;
        for (int bank = 0; bank <= 6; ++bank) {
            if (skipCols > 0) { --skipCols; row += padRight("", colW); continue; }
            const auto items = resourceCell(page, bank);
            std::string text;
            for (std::size_t i = 0; i < items.size(); ++i) { if (i) text += " / "; text += items[i]; }
            if (text.empty()) text = "\xC2\xB7";
            row += padRight(text, colW);
            if (text.size() > static_cast<std::size_t>(colW)) skipCols = static_cast<int>((text.size() - colW + colW - 1) / colW);
        }
        lines.push_back(row);
    }
    lines.push_back("");

    // View 4: physical contents (RAM only).
    lines.push_back("\xE2\x94\x80\xE2\x94\x80 contents (physical) \xE2\x94\x80\xE2\x94\x80");
    lines.push_back("internal RAM  $C000\xE2\x80\x93$FFFF");
    const auto internalRam = m_controller->debugInternalRamPC1600();
    if (internalRam.empty()) {
        lines.push_back("    (empty)");
    } else {
        const auto rows = hexRowsFrom(internalRam, 0xC000);
        if (rows.empty()) lines.push_back("    (all 00/FF)");
        else lines.insert(lines.end(), rows.begin(), rows.end());
    }

    ringWriteAll({joinLines(lines)});
}
