#include "MachineCodeLoadDialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <string>

MachineCodeLoadDialog::MachineCodeLoadDialog(QWidget* parent, machinecode::Target target, size_t length,
                                             bool needsAddress, uint32_t headerAddr,
                                             const std::vector<machinecode::Slot>& headerSlots, bool slot1Attached,
                                             bool slot2Attached)
    : QDialog(parent),
      m_target(target),
      m_length(length),
      m_slot1Attached(slot1Attached),
      m_slot2Attached(slot2Attached),
      m_address(headerAddr) {
    setWindowTitle(tr("Load Machine Code"));

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    layout->addLayout(form);

    if (needsAddress) {
        m_addressField = new QLineEdit(this);
        m_addressField->setPlaceholderText(tr("hex, e.g. &C0C5"));
        form->addRow(tr("Start address:"), m_addressField);
        connect(m_addressField, &QLineEdit::textChanged, this, &MachineCodeLoadDialog::refresh);
    } else {
        const QString addr = QStringLiteral("&") + QString::number(headerAddr, 16).toUpper();
        form->addRow(tr("Start address:"), new QLabel(tr("%1 (from the file header)").arg(addr), this));
    }
    form->addRow(tr("Length:"), new QLabel(tr("%1 bytes").arg(length), this));

    m_slotCombo = new QComboBox(this);
    form->addRow(tr("Slot:"), m_slotCombo);
    m_slotLabel = qobject_cast<QLabel*>(form->labelForField(m_slotCombo));

    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    layout->addWidget(m_hint);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Load"));
    layout->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (!needsAddress) setSlotChoices(headerSlots);
    refresh();
}

machinecode::Slot MachineCodeLoadDialog::slot() const {
    if (m_slotCombo->count() == 0) return machinecode::Slot::S0;
    return static_cast<machinecode::Slot>(m_slotCombo->currentData().toInt());
}

void MachineCodeLoadDialog::setSlotChoices(const std::vector<machinecode::Slot>& choices) {
    const QVariant previous = m_slotCombo->currentData();
    m_slotCombo->clear();
    for (machinecode::Slot s : choices) {
        QString text;
        switch (s) {
            case machinecode::Slot::S0: text = tr("S0 (internal RAM)"); break;
            case machinecode::Slot::S1: text = tr("S1 (memory slot 1)"); break;
            case machinecode::Slot::S2: text = tr("S2 (memory slot 2)"); break;
        }
        m_slotCombo->addItem(text, static_cast<int>(s));
    }
    const int keep = m_slotCombo->findData(previous);
    if (keep >= 0) m_slotCombo->setCurrentIndex(keep);
    // Only a choice between S1 and S2 is worth asking; a single fitting slot
    // follows from the address.
    const bool ask = m_target == machinecode::Target::PC1600 && choices.size() > 1;
    m_slotCombo->setVisible(ask);
    if (m_slotLabel) m_slotLabel->setVisible(ask);
}

void MachineCodeLoadDialog::refresh() {
    QString hint;
    bool valid = true;
    if (m_addressField) {
        uint32_t addr = 0;
        const std::string text = m_addressField->text().toStdString();
        if (!machinecode::parseHexAddress(text, &addr)) {
            valid = false;
            if (!m_addressField->text().trimmed().isEmpty()) hint = tr("Not a hex address (&0000-&FFFF).");
            setSlotChoices({});
        } else {
            m_address = addr;
            if (m_target == machinecode::Target::PC1600) {
                std::string why;
                const auto choices =
                    machinecode::pc1600SlotsFor(addr, m_length, m_slot1Attached, m_slot2Attached, &why);
                setSlotChoices(choices);
                if (choices.empty()) {
                    valid = false;
                    hint = QString::fromStdString(why);
                }
            } else if (static_cast<uint64_t>(addr) + m_length > 0x10000) {
                valid = false;
                hint = tr("The code runs past &FFFF.");
            }
        }
    }
    m_hint->setText(hint);
    m_hint->setVisible(!hint.isEmpty());
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
}
