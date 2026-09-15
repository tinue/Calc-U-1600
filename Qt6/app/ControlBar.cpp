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

// A thin vertical rule between control-bar groups (reset | module slots |
// model/ROM pickers | preset/settings | plotter toggles) so same-looking
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

    layout->addStretch();

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
        saveButton->setVisible(false);
        m_slot[i].saveButton = saveButton;
        layout->addWidget(saveButton);

        connect(combo, &QComboBox::currentIndexChanged, this, [this, slot](int index) {
            emit moduleSelected(slot, m_slot[slot - 1].combo->itemData(index).toString());
        });
        connect(saveButton, &QPushButton::clicked, this, [this, slot] { emit nameAndSaveRequested(slot); });

        if (slot == 1) m_slot2Separator = addSeparator(layout, this);
    }
    setSlot2Visible(false);

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
    m_openPresetButton = new QPushButton(tr("Load Preset…"), this);
    m_openPresetButton->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(m_openPresetButton);

    m_settingsButton = new QPushButton(tr("Settings…"), this);
    m_settingsButton->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(m_settingsButton);

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

    connect(resetButton, &QPushButton::clicked, this, [this, resetButton] {
        emit resetClicked(resetButton->lastClickWasCmd());
    });
    connect(m_modelCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        emit modelSelected(static_cast<Model>(m_modelCombo->itemData(index).toInt()));
    });
    connect(m_romCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        emit romRevisionSelected(static_cast<PC1500RomRevision>(m_romCombo->itemData(index).toInt()));
    });
    connect(m_openPresetButton, &QPushButton::clicked, this, [this] { emit openPresetRequested(); });
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
    QComboBox* combo = m_slot[slot - 1].combo;
    const QSignalBlocker blocker(combo);
    combo->clear();
    combo->addItem(tr("–empty–"), QString());
    for (const auto& e : bundled) combo->addItem(e.moduleName, e.moduleName);
    if (!instances.isEmpty()) {
        combo->insertSeparator(combo->count());
        for (const auto& e : instances) combo->addItem(e.moduleName, e.moduleName);
    }
    const int idx = combo->findData(selectedOrEmpty);
    combo->setCurrentIndex(idx >= 0 ? idx : 0);
}

void ControlBar::setSlotBatteryBacked(int slot, bool battery) {
    m_slot[slot - 1].saveButton->setVisible(battery);
}

void ControlBar::setSlot2Visible(bool visible) {
    m_slot[1].label->setVisible(visible);
    m_slot[1].combo->setVisible(visible);
    if (m_slot2Separator) m_slot2Separator->setVisible(visible);
    if (!visible) m_slot[1].saveButton->setVisible(false);
    // When becoming visible again, MainWindow re-syncs the save button's
    // state via setSlotBatteryBacked() right after re-populating the
    // combo, so no visibility is set here in that direction.
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
