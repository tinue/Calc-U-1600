#pragma once
#include <QDialog>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "MachineCodeFile.hpp"

class QDialogButtonBox;
class QLabel;
class QLineEdit;

// "Load Machine Code…" question dialog for a file without a header: asks
// the start address (see machinecode::plan()). On a PC-1600 it is pre-
// filled with the start of BASIC's program area (plan().defaultAddr) and
// the code always goes into that area -- machinecode::pc1600TargetFor().
// MainWindow skips the dialog when the file has a header.
class MachineCodeLoadDialog : public QDialog {
    Q_OBJECT
public:
    // `defaultAddr`: pre-filled address, 0 = none. `basicAreas`: the live
    // PC-1600 program area, used to check a typed address.
    MachineCodeLoadDialog(QWidget* parent, machinecode::Target target, size_t length, uint32_t defaultAddr,
                          const std::vector<machinecode::BasicArea>& basicAreas);

    uint32_t address() const { return m_address; }
    machinecode::Slot slot() const { return m_slot; }

private:
    void refresh();

    machinecode::Target m_target;
    size_t m_length;
    std::vector<machinecode::BasicArea> m_basicAreas;
    uint32_t m_address = 0;
    machinecode::Slot m_slot = machinecode::Slot::S0;

    QLineEdit* m_addressField = nullptr;
    QLabel* m_hint = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
};
