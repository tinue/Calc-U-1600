#include "ControlBar.hpp"

#include <QComboBox>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QStyle>

namespace {

// A thin vertical rule between control-bar groups (reset | settings |
// model/ROM pickers | module slots | plotter toggles) so same-looking
// widgets in adjacent groups -- most notably the two slots' identical save-
// icon buttons -- read as belonging to different groups instead of mushing
// into one undifferentiated row.
QFrame* addSeparator(QHBoxLayout* layout, QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line);
    return line;
}

// Qt on macOS reports the physical Cmd key as Qt::ControlModifier (it
// swaps Ctrl/Cmd by default to match platform convention, per Qt's own
// docs on QKeyEvent/keyboardModifiers). Named/isolated here so the one-line
// fix is obvious if that assumption turns out wrong on a given Qt/macOS
// combination.
bool isCmdHeld(Qt::KeyboardModifiers mods) {
    return mods.testFlag(Qt::ControlModifier);
}

// Small QPushButton subclass so the Reset button can tell a plain click
// from a Cmd-click.
class ResetButton : public QPushButton {
public:
    using QPushButton::QPushButton;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        m_cmdHeld = isCmdHeld(event->modifiers());
        QPushButton::mousePressEvent(event);
    }

public:
    bool lastClickWasCmd() const { return m_cmdHeld; }

private:
    bool m_cmdHeld = false;
};

// Shared by the memory-slot and floppy pickers: "–empty–", the bundled
// names, then (after a separator) the user's saved ones. A saved entry that
// shares a bundled name is left out -- lookup is bundled-first, so picking
// it would load the bundled one anyway.
void fillPicker(QComboBox* combo, const QStringList& bundled, const QStringList& saved,
                const QString& selectedOrEmpty) {
    const QSignalBlocker blocker(combo);
    combo->clear();
    combo->addItem(ControlBar::tr("–empty–"), QString());
    for (const auto& name : bundled) combo->addItem(name, name);
    bool separated = false;
    for (const auto& name : saved) {
        if (bundled.contains(name)) continue;
        if (!separated) {
            combo->insertSeparator(combo->count());
            separated = true;
        }
        combo->addItem(name, name);
    }
    const int idx = combo->findData(selectedOrEmpty);
    combo->setCurrentIndex(idx >= 0 ? idx : 0);
}

template <typename Entry, typename NameOf>
QStringList namesOf(const QVector<Entry>& entries, NameOf nameOf) {
    QStringList out;
    for (const auto& e : entries) out << nameOf(e);
    return out;
}

} // namespace

ControlBar::ControlBar(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);

    auto* resetButton = new ResetButton(tr("Reset"), this);
    m_resetButton = resetButton;
    // Never take keyboard focus from MainWindow (which owns physical-
    // keyboard typing) -- Qt's default focus policy for a button/combo box
    // varies by platform style, so this is set explicitly rather than left
    // to that default. Still fully mouse-clickable either way.
    m_resetButton->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(m_resetButton);

    addSeparator(layout, this);
    m_settingsButton = new QPushButton(tr("Settings…"), this);
    m_settingsButton->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(m_settingsButton);

    addSeparator(layout, this);
    m_modelCombo = new QComboBox(this);
    m_modelCombo->addItem(tr("PC-1500"), static_cast<int>(Model::PC1500));
    m_modelCombo->addItem(tr("PC-1500A"), static_cast<int>(Model::PC1500A));
    m_modelCombo->addItem(tr("PC-1600"), static_cast<int>(Model::PC1600));
    m_modelCombo->setCurrentIndex(1); // PC-1500A, matching MachineController's default
    m_modelCombo->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(m_modelCombo);

    m_romCombo = new QComboBox(this);
    m_romCombo->addItem(tr("A01"), static_cast<int>(PC1500RomRevision::A01));
    m_romCombo->addItem(tr("A03"), static_cast<int>(PC1500RomRevision::A03));
    m_romCombo->addItem(tr("A04"), static_cast<int>(PC1500RomRevision::A04));
    m_romCombo->setCurrentIndex(2); // A04, matching MachineController's default
    m_romCombo->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(m_romCombo);
    setRomPickerVisible(false); // PC-1500A is the default model (see m_modelCombo above)

    addSeparator(layout, this);
    for (int i = 0; i < 2; ++i) {
        const int slot = i + 1;
        // "1:"/"2:" labels so the two slots' otherwise-identical combo +
        // save-icon-button pairs are distinguishable at a glance.
        auto* label = new QLabel(tr("%1:").arg(slot), this);
        m_slot[i].label = label;
        layout->addWidget(label);

        auto* combo = new QComboBox(this);
        combo->setFocusPolicy(Qt::NoFocus);
        combo->setMinimumContentsLength(12);
        m_slot[i].combo = combo;
        layout->addWidget(combo);

        auto* saveButton = new QPushButton(this);
        saveButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
        saveButton->setToolTip(tr("Name & Save"));
        saveButton->setFocusPolicy(Qt::NoFocus);
        saveButton->setEnabled(false);  // always shown; see setSlotBatteryBacked()
        m_slot[i].saveButton = saveButton;
        layout->addWidget(saveButton);

        connect(combo, &QComboBox::currentIndexChanged, this, [this, slot](int index) {
            emit moduleSelected(slot, m_slot[slot - 1].combo->itemData(index).toString());
        });
        connect(saveButton, &QPushButton::clicked, this, [this, slot] { emit nameAndSaveRequested(slot); });

        if (slot == 1) m_slot2Separator = addSeparator(layout, this);
    }
    setSlot2Visible(false);

    // Left group is ordered most-common-first (Reset, Settings, model, slots)
    // so model-dependent widgets (ROM picker, slot 2) only change its tail
    // and switching models doesn't shift the rest; the plotter/floppy group
    // below is pinned to the right edge.
    layout->addStretch();

    addSeparator(layout, this);
    m_ce150Button = new QPushButton(tr("CE-150"), this);
    m_ce150Button->setCheckable(true);
    m_ce150Button->setFocusPolicy(Qt::NoFocus);
    m_ce150Button->setToolTip(tr("Attach/detach the CE-150 plotter (requires a power cycle)"));
    layout->addWidget(m_ce150Button);

    m_ce1600pButton = new QPushButton(tr("CE-1600P"), this);
    m_ce1600pButton->setCheckable(true);
    m_ce1600pButton->setFocusPolicy(Qt::NoFocus);
    m_ce1600pButton->setToolTip(tr("Attach/detach the CE-1600P plotter (requires a power cycle)"));
    layout->addWidget(m_ce1600pButton);
    setCe1600pVisible(false);

    // CE-1600F floppy disk picker -- attaches as a union with CE-1600P
    // (PC1600Machine::attachCE1600P()). Always shown on a PC-1600 (see
    // setFloppyVisible()'s comment) so the control bar doesn't jump around
    // as the plotter attaches/detaches; setFloppyEnabled() grays the whole
    // row out instead while the floppy isn't actually present.
    m_floppyLabel = new QLabel(tr("Disk:"), this);
    layout->addWidget(m_floppyLabel);

    m_floppyCombo = new QComboBox(this);
    m_floppyCombo->setFocusPolicy(Qt::NoFocus);
    m_floppyCombo->setMinimumContentsLength(12);
    layout->addWidget(m_floppyCombo);

    // Side toggle -- the software analogue of ejecting and flipping the
    // physical disk. Label shows "A"/"B" for whichever side currently
    // faces the head; clicking flips it.
    m_floppySideButton = new QPushButton(tr("A"), this);
    m_floppySideButton->setFocusPolicy(Qt::NoFocus);
    m_floppySideButton->setToolTip(tr("Eject and turn the disk over"));
    layout->addWidget(m_floppySideButton);

    m_floppySaveButton = new QPushButton(this);
    m_floppySaveButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    m_floppySaveButton->setToolTip(tr("Name & Save"));
    m_floppySaveButton->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(m_floppySaveButton);

    // The "green lamp" -- drive-active indicator. A plain colored dot via
    // stylesheet rather than an icon asset; red/gray reads fine at this
    // size and needs no bundled resource.
    m_floppyLampLabel = new QLabel(this);
    m_floppyLampLabel->setFixedWidth(14);
    m_floppyLampLabel->setAlignment(Qt::AlignCenter);
    m_floppyLampLabel->setToolTip(tr("Drive active -- wait for this to go dark before turning the disk over"));
    m_floppyLampLabel->setText(QStringLiteral("●"));  // filled circle
    layout->addWidget(m_floppyLampLabel);
    applyFloppyLampStyle();

    connect(m_floppyCombo, &QComboBox::currentIndexChanged, this,
            [this](int index) { emit floppyDiskSelected(m_floppyCombo->itemData(index).toString()); });
    connect(m_floppySaveButton, &QPushButton::clicked, this, [this] { emit floppyNameAndSaveRequested(); });
    connect(m_floppySideButton, &QPushButton::clicked, this, [this] { emit floppySideToggleRequested(); });

    setFloppyVisible(false);
    setFloppyEnabled(false);

    connect(resetButton, &QPushButton::clicked, this, [this, resetButton] {
        emit resetClicked(resetButton->lastClickWasCmd());
    });
    connect(m_modelCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        emit modelSelected(static_cast<Model>(m_modelCombo->itemData(index).toInt()));
    });
    connect(m_romCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        emit romRevisionSelected(static_cast<PC1500RomRevision>(m_romCombo->itemData(index).toInt()));
    });
    connect(m_settingsButton, &QPushButton::clicked, this, [this] { emit settingsRequested(); });
    // Buttons are checkable so their own click already toggled the visual
    // check state -- MainWindow will resync it (via setCe150State/
    // setCe1600pState) once PlotterController confirms the actual result,
    // so no manual setChecked() here.
    connect(m_ce150Button, &QPushButton::clicked, this, [this] { emit ce150ToggleRequested(); });
    connect(m_ce1600pButton, &QPushButton::clicked, this, [this] { emit ce1600pToggleRequested(); });
}

void ControlBar::setModel(Model model) {
    const QSignalBlocker blocker(m_modelCombo);
    m_modelCombo->setCurrentIndex(static_cast<int>(model));
}

void ControlBar::setRomRevision(PC1500RomRevision revision) {
    const QSignalBlocker blocker(m_romCombo);
    const int idx = m_romCombo->findData(static_cast<int>(revision));
    m_romCombo->setCurrentIndex(idx >= 0 ? idx : m_romCombo->count() - 1);
}

void ControlBar::setRomPickerVisible(bool visible) {
    m_romCombo->setVisible(visible);
}

void ControlBar::setModuleCombos(int slot, const QVector<MemoryModuleManager::ModuleEntry>& bundled,
                                  const QVector<MemoryModuleManager::ModuleEntry>& instances,
                                  const QString& selectedOrEmpty) {
    const auto name = [](const MemoryModuleManager::ModuleEntry& e) { return e.moduleName; };
    fillPicker(m_slot[slot - 1].combo, namesOf(bundled, name), namesOf(instances, name), selectedOrEmpty);
}

// Enables rather than shows/hides the button, so the control bar doesn't
// shift as modules change.
void ControlBar::setSlotBatteryBacked(int slot, bool battery) {
    m_slot[slot - 1].saveButton->setEnabled(battery);
}

void ControlBar::setSlot2Visible(bool visible) {
    m_slot[1].label->setVisible(visible);
    m_slot[1].combo->setVisible(visible);
    if (m_slot2Separator) m_slot2Separator->setVisible(visible);
    m_slot[1].saveButton->setVisible(visible);
}

void ControlBar::setCe150State(bool attached, bool enabled) {
    const QSignalBlocker blocker(m_ce150Button);
    m_ce150Button->setChecked(attached);
    m_ce150Button->setEnabled(enabled);
}

void ControlBar::setCe1600pState(bool attached, bool enabled) {
    const QSignalBlocker blocker(m_ce1600pButton);
    m_ce1600pButton->setChecked(attached);
    m_ce1600pButton->setEnabled(enabled);
}

void ControlBar::setCe1600pVisible(bool visible) {
    m_ce1600pButton->setVisible(visible);
}

void ControlBar::setFloppyCombo(const QVector<FloppyDiskManager::DiskEntry>& bundled,
                                const QVector<FloppyDiskManager::DiskEntry>& instances,
                                const QString& selectedOrEmpty) {
    const auto name = [](const FloppyDiskManager::DiskEntry& e) { return e.diskName; };
    fillPicker(m_floppyCombo, namesOf(bundled, name), namesOf(instances, name), selectedOrEmpty);
}

void ControlBar::setFloppyVisible(bool visible) {
    m_floppyLabel->setVisible(visible);
    m_floppyCombo->setVisible(visible);
    m_floppySaveButton->setVisible(visible);
    m_floppySideButton->setVisible(visible);
    m_floppyLampLabel->setVisible(visible);
}

void ControlBar::setFloppyEnabled(bool enabled) {
    m_floppyCombo->setEnabled(enabled);
    m_floppySaveButton->setEnabled(enabled);
    m_floppySideButton->setEnabled(enabled);
}

void ControlBar::setFloppySide(int side) {
    m_floppySideButton->setText(side ? tr("B") : tr("A"));
}

// Polled every frame tick -- only restyle on an actual change, since
// setStyleSheet() re-polishes the label.
void ControlBar::setFloppyMotorOn(bool on) {
    if (on == m_floppyMotorOn) return;
    m_floppyMotorOn = on;
    applyFloppyLampStyle();
}

void ControlBar::applyFloppyLampStyle() {
    m_floppyLampLabel->setStyleSheet(m_floppyMotorOn ? QStringLiteral("color: #2ecc40;")
                                                     : QStringLiteral("color: #888888;"));
}
