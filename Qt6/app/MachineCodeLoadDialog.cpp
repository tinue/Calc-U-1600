#include "MachineCodeLoadDialog.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

MachineCodeLoadDialog::MachineCodeLoadDialog(QWidget* parent, shared::MachineCodeLoad& load)
    : QDialog(parent), m_load(load) {
    setWindowTitle(tr("Load Machine Code"));
    setObjectName(QStringLiteral("dialog.machinecode"));
    // Wide enough that the wrapped notes read as sentences; the height
    // follows the text (refresh() re-fits it whenever the hint changes).
    setMinimumWidth(460);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    layout->addLayout(form);

    m_addressField = new QLineEdit(this);
    m_addressField->setPlaceholderText(QString::fromStdString(load.addressPlaceholder()));
    m_addressField->setText(QString::fromStdString(load.defaultAddressText()));
    form->addRow(tr("Start address:"), m_addressField);
    connect(m_addressField, &QLineEdit::textChanged, this, &MachineCodeLoadDialog::refresh);
    form->addRow(tr("Length:"), new QLabel(tr("%1 bytes").arg(load.length()), this));

    const std::string cpuNote = load.cpuNote();
    if (!cpuNote.empty()) {
        auto* note = new QLabel(QString::fromStdString(cpuNote), this);
        note->setWordWrap(true);
        layout->addWidget(note);
    }

    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    layout->addWidget(m_hint);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Load"));
    layout->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    resize(minimumWidth(), height());
    refresh();
}

void MachineCodeLoadDialog::refresh() {
    const shared::MachineCodeLoad::AddressCheck check = m_load.setAddress(m_addressField->text().toStdString());
    const QString hint = QString::fromStdString(check.hint);
    m_hint->setText(hint);
    m_hint->setVisible(!hint.isEmpty());
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(check.valid);
    // Re-fit the height to the wrapped text at the current width, or a long
    // hint gets clipped.
    layout()->activate();
    const int h = layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width()) : sizeHint().height();
    resize(width(), std::max(h, layout()->totalMinimumSize().height()));
}
