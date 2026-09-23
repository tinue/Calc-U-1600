#include "Ce158PrinterWidget.hpp"
#include "ChromeColors.hpp"
#include "MachineController.hpp"
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextCursor>
#include <QVBoxLayout>

Ce158PrinterWidget::Ce158PrinterWidget(MachineController* controller, QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_header = new QLabel(tr("CE-158"), this);
    m_header->setAlignment(Qt::AlignCenter);
    QFont headerFont = m_header->font();
    headerFont.setBold(true);
    headerFont.setPointSize(headerFont.pointSize() - 1);
    m_header->setFont(headerFont);
    outer->addWidget(m_header);

    m_serialLabel = new QLabel(this);
    m_serialLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_serialLabel->setWordWrap(true);
    m_serialLabel->setContentsMargins(8, 4, 8, 4);
    m_serialLabel->setFocusPolicy(Qt::NoFocus);
    outer->addWidget(m_serialLabel);

    m_output = new QPlainTextEdit(this);
    m_output->setReadOnly(true);
    m_output->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_output->setPlaceholderText(tr("Parallel-port output (OPN \"LPRT\" / PRINT#-9) appears here."));
    // Never take keyboard focus from MainWindow, which owns typing.
    m_output->setFocusPolicy(Qt::NoFocus);
    outer->addWidget(m_output, /*stretch=*/1);

    m_buttonBar = new QWidget(this);
    auto* buttonBarLayout = new QHBoxLayout(m_buttonBar);
    buttonBarLayout->setContentsMargins(10, 8, 10, 8);
    buttonBarLayout->setSpacing(8);
    m_saveButton = new QPushButton(tr("Save…"), m_buttonBar);
    m_saveButton->setToolTip(tr("Save the printed bytes, exactly as sent, to a file."));
    m_saveButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton = new QPushButton(tr("Clear"), m_buttonBar);
    m_clearButton->setToolTip(tr("Tear off the paper (clears the printed output)."));
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    buttonBarLayout->addWidget(m_saveButton);
    buttonBarLayout->addWidget(m_clearButton);
    buttonBarLayout->addStretch(1);
    outer->addWidget(m_buttonBar);

    connect(m_saveButton, &QPushButton::clicked, this, &Ce158PrinterWidget::saveToFile);
    connect(m_clearButton, &QPushButton::clicked, this, &Ce158PrinterWidget::clearOutput);

    applyChrome();
    onFrameTick();
}

void Ce158PrinterWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
        applyChrome();
    }
}

void Ce158PrinterWidget::applyChrome() {
    const ChromeColors c = ChromeColors::forWidget(this);
    m_header->setStyleSheet(ChromeStyle::header(c));
    m_serialLabel->setStyleSheet(QString("background-color: %1; color: %2;")
                                     .arg(cssRgba(c.buttonBarBackground), cssRgba(c.outputText)));
    m_output->setStyleSheet(QString("QPlainTextEdit { background-color: %1; color: %2; border: 1px solid %3; }")
                                .arg(cssRgba(c.outputBackground), cssRgba(c.outputText), cssRgba(c.outputBorder)));
    m_buttonBar->setStyleSheet(ChromeStyle::buttonBar(c));
    const QString buttonStyle =
        QString("QPushButton { border: none; %1 }"
                "QPushButton:hover:!disabled { background-color: %2; }"
                "QPushButton:pressed, QPushButton:hover:pressed { background-color: %3; }"
                "QPushButton:disabled { background-color: %4; color: %5; }")
            .arg(ChromeStyle::pillCore(c.pillBackground, c.pillText),
                 cssRgba(c.pillBackground.lighter(115)), cssRgba(c.pillBackground.darker(130)),
                 cssRgba(c.pillBackgroundOff), cssRgba(c.pillText));
    m_saveButton->setStyleSheet(buttonStyle);
    m_clearButton->setStyleSheet(buttonStyle);
}

void Ce158PrinterWidget::onFrameTick() {
    const std::vector<std::uint8_t> bytes = m_controller->drainCE158PrinterOutput();
    if (!bytes.empty()) {
        appendBytes(QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())));
    }
    const QString path = m_controller->ce158SerialLinkStatus();
    if (path != m_serialPath || m_serialLabel->text().isEmpty()) {
        m_serialPath = path;
        m_serialLabel->setText(path.isEmpty() ? tr("RS-232C: no host port (the pseudo-terminal could not be opened)")
                                              : tr("RS-232C: %1").arg(path));
    }
    m_saveButton->setEnabled(!m_printed.isEmpty());
    m_clearButton->setEnabled(!m_printed.isEmpty());
}

void Ce158PrinterWidget::appendBytes(const QByteArray& bytes) {
    m_printed.append(bytes);
    QString text;
    for (const char ch : bytes) {
        const auto b = static_cast<unsigned char>(ch);
        if (b == '\r') continue;
        if (b == '\n' || (b >= 0x20 && b < 0x7F)) text += QChar(b);
        else text += QString("<%1>").arg(b, 2, 16, QChar('0')).toUpper();
    }
    QScrollBar* bar = m_output->verticalScrollBar();
    const bool follow = bar->value() >= bar->maximum() - 4;
    QTextCursor cursor(m_output->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    if (follow) bar->setValue(bar->maximum());
}

void Ce158PrinterWidget::saveToFile() {
    const QString path = QFileDialog::getSaveFileName(this, tr("Save Printer Output"), QStringLiteral("ce158-printout.txt"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(m_printed) != m_printed.size()) {
        QMessageBox::warning(this, tr("Save Printer Output"), tr("Could not write %1.").arg(path));
    }
}

void Ce158PrinterWidget::clearOutput() {
    m_printed.clear();
    m_output->clear();
    m_saveButton->setEnabled(false);
    m_clearButton->setEnabled(false);
}
