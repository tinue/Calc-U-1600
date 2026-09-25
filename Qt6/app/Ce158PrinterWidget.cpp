#include "Ce158PrinterWidget.hpp"
#include "AppPaths.hpp"
#include "ChromeColors.hpp"
#include "MachineController.hpp"
#include "Connector/Ce158Card.hpp"
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
    m_saveButton->setObjectName(QStringLiteral("ce158printer.save"));
    m_clearButton = new QPushButton(tr("Clear"), m_buttonBar);
    m_clearButton->setToolTip(tr("Tear off the paper (clears the printed output)."));
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton->setObjectName(QStringLiteral("ce158printer.clear"));
    buttonBarLayout->addWidget(m_saveButton);
    buttonBarLayout->addWidget(m_clearButton);
    buttonBarLayout->addStretch(1);
    outer->addWidget(m_buttonBar);

    connect(m_saveButton, &QPushButton::clicked, this, &Ce158PrinterWidget::saveToFile);
    connect(m_clearButton, &QPushButton::clicked, this, &Ce158PrinterWidget::clearOutput);
    connect(m_controller, &MachineController::serialLinksMoved, this, &Ce158PrinterWidget::refreshSerialLabel);

    applyChrome();
    updateButtons();
    refreshSerialLabel();
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
    const QString buttonStyle = ChromeStyle::pillPushButton(c);
    m_saveButton->setStyleSheet(buttonStyle);
    m_clearButton->setStyleSheet(buttonStyle);
}

void Ce158PrinterWidget::onFrameTick() {
    const std::vector<std::uint8_t> bytes = m_controller->drainCE158PrinterOutput();
    if (!bytes.empty()) {
        appendBytes(QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())));
    }
}

void Ce158PrinterWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refreshSerialLabel();
}

void Ce158PrinterWidget::refreshSerialLabel() {
    const QString path = m_controller->ce158SerialLinkStatus();
    m_serialLabel->setText(path.isEmpty() ? tr("RS-232C: no host port (the pseudo-terminal could not be opened)")
                                          : tr("RS-232C: %1").arg(AppPaths::displayPath(path)));
}

void Ce158PrinterWidget::updateButtons() {
    m_saveButton->setEnabled(!m_printed.isEmpty());
    m_clearButton->setEnabled(!m_printed.isEmpty());
}

void Ce158PrinterWidget::appendBytes(const QByteArray& bytes) {
    const bool wasEmpty = m_printed.isEmpty();
    m_printed.append(bytes);
    if (wasEmpty) updateButtons();
    const QString text = QString::fromStdString(ce158PrintableText(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()), static_cast<std::size_t>(bytes.size())));
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
    updateButtons();
}
