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
// the start address (see machinecode::plan()). On a PC-1600 the machine's
// MODE decides the address space (MODE 0: a Z-80 address for Z-80 code,
// MODE 1: an LH5803 address for LH5801 code -- the dialog says which), and
// MODE / TITLE decide the target (machinecode::pc1600TargetFor()); it is
// pre-filled with the start of the selected program area. MainWindow skips
// the dialog when the file has a header.
class MachineCodeLoadDialog : public QDialog {
    Q_OBJECT
public:
    // `defaultAddr`: pre-filled address in `cpu`'s space, 0 = none.
    // `state`: the live PC-1600 MODE / TITLE / program areas.
    MachineCodeLoadDialog(QWidget* parent, machinecode::Target target, size_t length, uint32_t defaultAddr,
                          const machinecode::PC1600State& state, machinecode::Cpu cpu);

    uint32_t address() const { return m_address; }        // as typed, in the CPU's space
    uint32_t busAddress() const { return m_busAddress; }  // where the bytes go (Z-80 address)
    machinecode::Slot slot() const { return m_slot; }

private:
    void refresh();

    machinecode::Target m_target;
    size_t m_length;
    machinecode::PC1600State m_state;
    machinecode::Cpu m_cpu;
    uint32_t m_address = 0;
    uint32_t m_busAddress = 0;
    machinecode::Slot m_slot = machinecode::Slot::S0;

    QLineEdit* m_addressField = nullptr;
    QLabel* m_hint = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
};
