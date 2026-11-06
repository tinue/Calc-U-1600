#pragma once
#include <QDialog>

#include "Shared/MachineCodeLoad.hpp"

class QDialogButtonBox;
class QLabel;
class QLineEdit;

// "Load Machine Code…" question dialog for a file without a header: asks
// the start address. What it says and whether an address is valid come
// from shared::MachineCodeLoad (MODE / TITLE decide the address space and
// the target on a PC-1600); a valid address is set on `load`. MainWindow
// skips the dialog when the file has a header.
class MachineCodeLoadDialog : public QDialog {
    Q_OBJECT
public:
    MachineCodeLoadDialog(QWidget* parent, shared::MachineCodeLoad& load);

private:
    void refresh();

    shared::MachineCodeLoad& m_load;
    QLineEdit* m_addressField = nullptr;
    QLabel* m_hint = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
};
