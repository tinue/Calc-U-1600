#include "MachineCodeLoadDialog.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <string>

MachineCodeLoadDialog::MachineCodeLoadDialog(QWidget* parent, machinecode::Target target, size_t length,
                                             uint32_t defaultAddr,
                                             const std::vector<machinecode::BasicArea>& basicAreas)
    : QDialog(parent), m_target(target), m_length(length), m_basicAreas(basicAreas) {
    setWindowTitle(tr("Load Machine Code"));

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    layout->addLayout(form);

    m_addressField = new QLineEdit(this);
    m_addressField->setPlaceholderText(tr("hex, e.g. &C0C5"));
    if (defaultAddr != 0)
        m_addressField->setText(QStringLiteral("&") + QString::number(defaultAddr, 16).toUpper());
    form->addRow(tr("Start address:"), m_addressField);
    connect(m_addressField, &QLineEdit::textChanged, this, &MachineCodeLoadDialog::refresh);
    form->addRow(tr("Length:"), new QLabel(tr("%1 bytes").arg(length), this));

    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    layout->addWidget(m_hint);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Load"));
    layout->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    refresh();
}

void MachineCodeLoadDialog::refresh() {
    QString hint;
    bool valid = true;
    uint32_t addr = 0;
    const std::string text = m_addressField->text().toStdString();
    if (!machinecode::parseHexAddress(text, &addr)) {
        valid = false;
        if (!m_addressField->text().trimmed().isEmpty()) hint = tr("Not a hex address (&0000-&FFFF).");
    } else {
        m_address = addr;
        if (m_target == machinecode::Target::PC1600) {
            std::string why;
            if (!machinecode::pc1600TargetFor(addr, m_length, m_basicAreas, &m_slot, &why)) {
                valid = false;
                hint = QString::fromStdString(why);
            } else if (m_slot != machinecode::Slot::S0) {
                hint = tr("Goes into the slot %1 module, where BASIC's program area starts.")
                           .arg(m_slot == machinecode::Slot::S1 ? 1 : 2);
            }
        } else if (static_cast<uint64_t>(addr) + m_length > 0x10000) {
            valid = false;
            hint = tr("The code runs past &FFFF.");
        }
    }
    m_hint->setText(hint);
    m_hint->setVisible(!hint.isEmpty());
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
}
