#pragma once
#include <QDialog>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "MachineCodeFile.hpp"

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;

// "Load Machine Code…" question dialog: asks only what the file doesn't
// answer -- the start address when it has no header, and the PC-1600 slot
// when more than one fits (see machinecode::plan()). MainWindow skips it
// entirely when there's nothing to ask.
class MachineCodeLoadDialog : public QDialog {
    Q_OBJECT
public:
    // `headerAddr` / `headerSlots`: from the file header and plan() when
    // `needsAddress` is false. `slot1Attached` / `slot2Attached`: the live
    // PC-1600 choices, used to work out the fitting choices for a typed address.
    MachineCodeLoadDialog(QWidget* parent, machinecode::Target target, size_t length, bool needsAddress,
                          uint32_t headerAddr, const std::vector<machinecode::Slot>& headerSlots,
                          bool slot1Attached, bool slot2Attached);

    uint32_t address() const { return m_address; }
    machinecode::Slot slot() const;

private:
    void refresh();
    void setSlotChoices(const std::vector<machinecode::Slot>& choices);

    machinecode::Target m_target;
    size_t m_length;
    bool m_slot1Attached;
    bool m_slot2Attached;
    uint32_t m_address = 0;

    QLineEdit* m_addressField = nullptr;
    QLabel* m_slotLabel = nullptr;
    QComboBox* m_slotCombo = nullptr;
    QLabel* m_hint = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
};
