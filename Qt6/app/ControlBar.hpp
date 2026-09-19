#pragma once
#include <QWidget>
#include <QVector>
#include "MachineController.hpp"
#include "MemoryModuleManager.hpp"
#include "FloppyDiskManager.hpp"

class QComboBox;
class QLabel;
class QPushButton;

// Reset + model picker + per-slot memory-module pickers + Settings,
// directly under the faceplate.
class ControlBar : public QWidget {
    Q_OBJECT
public:
    explicit ControlBar(QWidget* parent = nullptr);

    void setModel(Model model); // reflect an externally-driven model change

    // ROM-revision picker -- PC-1500 (plain) only; PC-1500A is A04-only
    // (see PC1500Variant.hpp) so there's nothing worth picking there, and
    // PC-1600 has its own fixed ROM set entirely. Mirrors setModel()'s
    // QSignalBlocker'd external-resync shape.
    void setRomRevision(PC1500RomRevision revision);
    void setRomPickerVisible(bool visible);

    // Resyncs slot `slot`'s (1 or 2) combo box: bundled entries, then a
    // separator, then instance entries, with a leading "-empty-" item;
    // `selectedOrEmpty` picks the current item without re-emitting
    // moduleSelected (QSignalBlocker'd, like setModel()).
    void setModuleCombos(int slot, const QVector<MemoryModuleManager::ModuleEntry>& bundled,
                         const QVector<MemoryModuleManager::ModuleEntry>& instances,
                         const QString& selectedOrEmpty);
    void setSlotSaveEnabled(int slot, bool enabled);   // Name & Save offered (unsaved battery card)
    void setSlot2Visible(bool visible);                // PC1600 vs PC1500/1500A

    // Plotter toggle buttons -- checked state reflects live attachment,
    // enabled state reflects mutual exclusion (the other plotter attached)
    // and PlotterController's busy/power-cycling state. Never re-emits
    // ce150ToggleRequested/ce1600pToggleRequested (QSignalBlocker'd, like
    // setModel()). CE-1600P only exists on PC-1600.
    void setCe150State(bool attached, bool enabled);
    void setCe1600pState(bool attached, bool enabled);
    void setCe1600pVisible(bool visible);

    // CE-1600F floppy disk picker -- same combo+save-button shape as a
    // memory slot (setModuleCombos/setSlotSaveEnabled above). Always
    // shown on a PC-1600 (setFloppyVisible) so the control bar doesn't
    // jump around as the CE-1600P attaches/detaches -- setFloppyEnabled
    // grays the row out instead while the floppy (which attaches as a
    // union with the CE-1600P) isn't actually present.
    void setFloppyCombo(const QVector<FloppyDiskManager::DiskEntry>& bundled,
                        const QVector<FloppyDiskManager::DiskEntry>& instances, const QString& selectedOrEmpty);
    void setFloppyVisible(bool visible);   // PC-1600 vs PC-1500/1500A
    void setFloppyEnabled(bool enabled);   // CE-1600P attached vs not
    void setFloppySaveEnabled(bool enabled); // Name & Save offered (unsaved disk in the drive)
    // "A"/"B" -- the side currently facing the head (CE1600FCard::side()).
    void setFloppySide(int side);
    // The "green lamp": true while the drive motor is spinning -- the user
    // manual's own cue for when it's safe to eject and flip the disk.
    void setFloppyMotorOn(bool on);

signals:
    void modelSelected(Model model);
    void romRevisionSelected(PC1500RomRevision revision);
    void resetClicked(bool allReset); // allReset == Cmd-click
    void moduleSelected(int slot, QString moduleNameOrEmpty); // "" => -empty-
    void nameAndSaveRequested(int slot);
    void settingsRequested();
    void ce150ToggleRequested();
    void ce1600pToggleRequested();
    void floppyDiskSelected(QString diskNameOrEmpty); // "" => no disk
    void floppyNameAndSaveRequested();
    void floppySideToggleRequested();

private:
    QComboBox* m_modelCombo = nullptr;
    QComboBox* m_romCombo = nullptr;
    QPushButton* m_resetButton = nullptr;
    QPushButton* m_settingsButton = nullptr;
    QPushButton* m_ce150Button = nullptr;
    QPushButton* m_ce1600pButton = nullptr;
    QLabel* m_floppyLabel = nullptr;
    QComboBox* m_floppyCombo = nullptr;
    QPushButton* m_floppySaveButton = nullptr;
    QPushButton* m_floppySideButton = nullptr;
    QLabel* m_floppyLampLabel = nullptr;
    bool m_floppyMotorOn = false;
    void applyFloppyLampStyle();

    struct SlotWidgets {
        QLabel* label = nullptr;           // "1:" / "2:", hidden together with the slot
        QComboBox* combo = nullptr;
        QPushButton* saveButton = nullptr; // hidden unless battery-backed
    };
    SlotWidgets m_slot[2]; // slot 1 = [0], slot 2 = [1]
    QWidget* m_slot2Separator = nullptr; // hidden together with slot 2 (PC1500/1500A)
};
