#pragma once
#include <QDialog>

class MachineController;

// Settings dialog: titled sections (General, Default presets, Storage,
// Tracing, Serial port), one grid row per setting -- label, the *effective*
// current value, and Change/Reset buttons (Reset is enabled only while a
// user override is stored). QSettings-backed (see AppSettings.hpp); writes
// happen immediately on each change, so there's no separate OK/Cancel
// semantics -- just one "Close" button.
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
