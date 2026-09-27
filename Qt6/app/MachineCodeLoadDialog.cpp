#include "MachineCodeLoadDialog.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <string>

#include "PC1600/PC1600ProgramPlacement.hpp"

MachineCodeLoadDialog::MachineCodeLoadDialog(QWidget* parent, machinecode::Target target, size_t length,
                                             uint32_t defaultAddr, const machinecode::PC1600State& state,
                                             machinecode::Cpu cpu)
    : QDialog(parent), m_target(target), m_length(length), m_state(state), m_cpu(cpu) {
    setWindowTitle(tr("Load Machine Code"));
    setObjectName(QStringLiteral("dialog.machinecode"));

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    layout->addLayout(form);

    const bool pc1600 = target == machinecode::Target::PC1600;
    const bool lh5803 = pc1600 && cpu == machinecode::Cpu::LH5803;
    m_addressField = new QLineEdit(this);
    // An example from the target's own map: the start of BASIC's program
    // area on the PC-1600 (in the MODE's address space), the classic
    // machine-code spot on the PC-1500.
    m_addressField->setPlaceholderText(!pc1600 ? tr("hex, e.g. &7C01")
                                               : lh5803 ? tr("hex, e.g. &40C5") : tr("hex, e.g. &C0C5"));
    if (defaultAddr != 0)
        m_addressField->setText(QStringLiteral("&") + QString::number(defaultAddr, 16).toUpper());
    form->addRow(tr("Start address:"), m_addressField);
    connect(m_addressField, &QLineEdit::textChanged, this, &MachineCodeLoadDialog::refresh);
    form->addRow(tr("Length:"), new QLabel(tr("%1 bytes").arg(length), this));

    if (pc1600) {
        auto* cpuNote = new QLabel(lh5803 ? tr("MODE 1: LH5801 code is assumed. The address is in the PC-1500 "
                                               "(LH5803) address space, where &4000-&7FFF is the internal RAM.")
                                          : tr("MODE 0: Z-80 code is assumed. The address is a Z-80 address."),
                                   this);
        cpuNote->setWordWrap(true);
        layout->addWidget(cpuNote);
    }

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
    } else if (m_target == machinecode::Target::PC1600) {
        m_address = addr;
        const bool lh5803 = m_cpu == machinecode::Cpu::LH5803;
        if (lh5803 && addr + m_length > 0x8000) {
            valid = false;
            hint = tr("The LH5803's RAM is &0000-&7FFF.");
        } else {
            m_busAddress = lh5803 ? pc1600::lh5803ToZ80(static_cast<uint16_t>(addr)) : addr;
            std::string why;
            const std::string work = machinecode::pc1600WorkAreaProblem(m_busAddress, m_length, m_cpu);
            if (!machinecode::pc1600TargetFor(m_busAddress, m_length, m_state, &m_slot, &why)) {
                valid = false;
                hint = QString::fromStdString(why);
            } else if (!work.empty()) {
                valid = false;
                hint = QString::fromStdString(work);
            } else if (m_slot != machinecode::Slot::S0) {
                hint = m_state.title != 0
                           ? tr("Goes into the slot %1 program module, the selected program area.")
                                 .arg(m_slot == machinecode::Slot::S1 ? 1 : 2)
                           : tr("Goes into the slot %1 module, where BASIC's program area starts.")
                                 .arg(m_slot == machinecode::Slot::S1 ? 1 : 2);
            }
        }
    } else {
        m_address = addr;
        m_busAddress = addr;
        if (static_cast<uint64_t>(addr) + m_length > 0x10000) {
            valid = false;
            hint = tr("The code runs past &FFFF.");
        }
    }
    m_hint->setText(hint);
    m_hint->setVisible(!hint.isEmpty());
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(valid);
}
