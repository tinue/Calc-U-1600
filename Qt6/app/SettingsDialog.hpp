#pragma once
#include <QDialog>

class MachineController;

// Minimal Settings dialog: shows the *effective* current battery-card
// instance save directory and lets the user override or reset it.
// QSettings-backed (see AppSettings.hpp); writes happen immediately on
// each button click, so there's no separate OK/Cancel semantics -- just
// one "Close" button.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    // `controller` is used for the Serial Port section only (relinking a
    // live PtySerialLink and showing its current path). That section is
    // compiled out on Windows entirely (see .cpp), so `controller` may be
    // null there without effect.
    explicit SettingsDialog(MachineController* controller, QWidget* parent = nullptr);

private:
    MachineController* m_controller = nullptr; // not owned
};
