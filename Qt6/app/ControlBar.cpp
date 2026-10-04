#include "ControlBar.hpp"

#include <QComboBox>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QGuiApplication>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionButton>

namespace {

// A thin vertical rule between control-bar groups (model picker | module slots | peripheral toggles) so same-looking
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

// Shared by the memory-slot and floppy pickers: "–empty–", the templates
// (bundled and the user's own), then (after a separator) the user's saved
// instances, then (after another) the ROM modules -- memory slots only.
void fillPicker(QComboBox* combo, const QStringList& templates, const QStringList& saved,
                const QString& selectedOrEmpty, const QStringList& roms = {}) {
    const QSignalBlocker blocker(combo);
    combo->clear();
    combo->addItem(ControlBar::tr("–empty–"), QString());
    for (const auto& name : templates) combo->addItem(name, name);
    if (!saved.isEmpty()) combo->insertSeparator(combo->count());
    for (const auto& name : saved) combo->addItem(name, name);
    if (!roms.isEmpty()) combo->insertSeparator(combo->count());
    for (const auto& name : roms) combo->addItem(name, name);
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
    // objectNames ("controlbar.*") are the handles screenshot scenarios
    // address widgets by -- see docs/developer/screenshots/README.md.
    setObjectName(QStringLiteral("controlbar"));
    auto* layout = new QHBoxLayout(this);

    // Every widget here is NoFocus: never take keyboard focus from
    // MainWindow (which owns physical-keyboard typing) -- Qt's default focus
    // policy for a button/combo box varies by platform style, so it is set
    // explicitly. Still fully mouse-clickable either way.
    m_modelCombo = new QComboBox(this);
    m_modelCombo->addItem(tr("PC-1500"), static_cast<int>(Model::PC1500));
    m_modelCombo->addItem(tr("PC-1500A"), static_cast<int>(Model::PC1500A));
    m_modelCombo->addItem(tr("PC-1600"), static_cast<int>(Model::PC1600));
    m_modelCombo->setCurrentIndex(1); // PC-1500A, matching MachineController's default
    m_modelCombo->setFocusPolicy(Qt::NoFocus);
    m_modelCombo->setObjectName(QStringLiteral("controlbar.model"));
    layout->addWidget(m_modelCombo);
    // The ROM versions rarely change, so they live in the Machine menu only.

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
        combo->setObjectName(QStringLiteral("controlbar.slot%1").arg(slot));
        m_slot[i].combo = combo;
        layout->addWidget(combo);

        auto* saveButton = new QPushButton(this);
        saveButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
        saveButton->setToolTip(tr("Name & Save"));
        saveButton->setFocusPolicy(Qt::NoFocus);
        saveButton->setEnabled(false);  // always shown; see setSlotSaveEnabled()
        saveButton->setObjectName(QStringLiteral("controlbar.slot%1.save").arg(slot));
        m_slot[i].saveButton = saveButton;
        layout->addWidget(saveButton);

        connect(combo, &QComboBox::currentIndexChanged, this, [this, slot](int index) {
            emit moduleSelected(slot, m_slot[slot - 1].combo->itemData(index).toString());
        });
        connect(saveButton, &QPushButton::clicked, this, [this, slot] { emit nameAndSaveRequested(slot); });

        if (slot == 1) m_slot2Separator = addSeparator(layout, this);
    }
    setSlot2Visible(false);

    // Left group is ordered most-common-first (model, slots)
    // so model-dependent widgets (slot 2) only change its tail
    // and switching models doesn't shift the rest; the plotter/floppy group
    // below is pinned to the right edge.
    layout->addStretch();

    addSeparator(layout, this);
    m_ce150Button = new QPushButton(tr("CE-150"), this);
    m_ce150Button->setCheckable(true);
    m_ce150Button->setFocusPolicy(Qt::NoFocus);
    m_ce150Button->setObjectName(QStringLiteral("controlbar.ce150"));
    m_ce150Button->setToolTip(tr("Attach/detach the CE-150 plotter (requires a power cycle)"));
    layout->addWidget(m_ce150Button);

    m_ce158Button = new QPushButton(tr("CE-158"), this);
    m_ce158Button->setCheckable(true);
    m_ce158Button->setFocusPolicy(Qt::NoFocus);
    m_ce158Button->setObjectName(QStringLiteral("controlbar.ce158"));
    m_ce158Button->setToolTip(tr("Attach/detach the CE-158 RS-232C/parallel interface (requires a power cycle)"));
    layout->addWidget(m_ce158Button);

    m_ce1600pButton = new QPushButton(tr("CE-1600P"), this);
    m_ce1600pButton->setCheckable(true);
    m_ce1600pButton->setFocusPolicy(Qt::NoFocus);
    m_ce1600pButton->setObjectName(QStringLiteral("controlbar.ce1600p"));
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
    m_floppyCombo->setMinimumContentsLength(9);
    m_floppyCombo->setObjectName(QStringLiteral("controlbar.floppy"));
    layout->addWidget(m_floppyCombo);

    // Side toggle -- the software analogue of ejecting and flipping the
    // physical disk. Label shows "A"/"B" for whichever side currently
    // faces the head; clicking flips it.
    m_floppySideButton = new QPushButton(tr("A"), this);
    m_floppySideButton->setFocusPolicy(Qt::NoFocus);
    m_floppySideButton->setToolTip(tr("Eject and turn the disk over"));
    m_floppySideButton->setObjectName(QStringLiteral("controlbar.floppy.side"));
    // Compact: one letter / one icon, so no wider than the diskette symbol --
    // unless the style insets a button's text further than that (macOS: 12 px
    // a side, which clipped the letter), so the letter itself still fits.
    const int compactWidth = m_floppySideButton->iconSize().width() + 12;
    QStyleOptionButton sideOption;
    sideOption.initFrom(m_floppySideButton);
    // The real size, not the not-yet-laid-out default: macOS picks its inset by button size.
    sideOption.rect.setSize({compactWidth, m_floppySideButton->sizeHint().height()});
    const int textInset =
        sideOption.rect.width() -
        style()->subElementRect(QStyle::SE_PushButtonContents, &sideOption, m_floppySideButton).width();
    const QFontMetrics metrics = m_floppySideButton->fontMetrics();
    const int letterWidth = qMax(metrics.horizontalAdvance(tr("A")), metrics.horizontalAdvance(tr("B")));
    m_floppySideButton->setFixedWidth(qMax(compactWidth, letterWidth + textInset));
    layout->addWidget(m_floppySideButton);

    m_floppySaveButton = new QPushButton(this);
    m_floppySaveButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    m_floppySaveButton->setToolTip(tr("Name & Save"));
    m_floppySaveButton->setFocusPolicy(Qt::NoFocus);
    m_floppySaveButton->setFixedWidth(compactWidth);
    m_floppySaveButton->setObjectName(QStringLiteral("controlbar.floppy.save"));
    layout->addWidget(m_floppySaveButton);

    // The "green lamp" -- drive-active indicator. A plain colored dot via
    // stylesheet rather than an icon asset; red/gray reads fine at this
    // size and needs no bundled resource.
    m_floppyLampLabel = new QLabel(this);
    m_floppyLampLabel->setFixedWidth(14);
    m_floppyLampLabel->setObjectName(QStringLiteral("controlbar.floppy.lamp"));
    m_floppyLampLabel->setAlignment(Qt::AlignCenter);
    m_floppyLampLabel->setToolTip(tr("Drive active -- wait for this to go dark before turning the disk over"));
    m_floppyLampLabel->setText(QStringLiteral("●"));  // filled circle
    layout->addWidget(m_floppyLampLabel);
    applyLampStyle(m_floppyLampLabel, false);

    connect(m_floppyCombo, &QComboBox::currentIndexChanged, this,
            [this](int index) { emit floppyDiskSelected(m_floppyCombo->itemData(index).toString()); });
    connect(m_floppySaveButton, &QPushButton::clicked, this, [this] { emit floppyNameAndSaveRequested(); });
    connect(m_floppySideButton, &QPushButton::clicked, this, [this] { emit floppySideToggleRequested(); });

    setFloppyVisible(false);
    setFloppyEnabled(false);
    setFloppySaveEnabled(false);

    // Cassette bay -- the tape follows the interface's remote relay, so
    // putting one in is all the user does; CLOAD / CSAVE start the motor.
    m_tapeSeparator = addSeparator(layout, this);
    m_tapeTitle = new QLabel(tr("Tape:"), this);
    layout->addWidget(m_tapeTitle);

    m_tapeCombo = new QComboBox(this);
    m_tapeCombo->setFocusPolicy(Qt::NoFocus);
    m_tapeCombo->setMinimumContentsLength(9);
    m_tapeCombo->setToolTip(tr("A tape to play (CLOAD)"));
    m_tapeCombo->setObjectName(QStringLiteral("controlbar.tape"));
    layout->addWidget(m_tapeCombo);

    m_tapeSaveButton = new QPushButton(this);
    m_tapeSaveButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    m_tapeSaveButton->setToolTip(tr("A new tape to record onto (CSAVE)"));
    m_tapeSaveButton->setFocusPolicy(Qt::NoFocus);
    m_tapeSaveButton->setFixedWidth(compactWidth);
    m_tapeSaveButton->setObjectName(QStringLiteral("controlbar.tape.save"));
    layout->addWidget(m_tapeSaveButton);
    // activated, not currentIndexChanged: picking the tape that's already
    // shown counts too -- a recording picked from the list goes back in to
    // play, and a playing tape picked again starts over, rewound.
    connect(m_tapeCombo, &QComboBox::activated, this,
            [this](int index) { emit tapeSelected(m_tapeCombo->itemData(index).toString()); });
    connect(m_tapeSaveButton, &QPushButton::clicked, this, [this] { emit tapeSaveRequested(); });

    m_tapeLabel = new QLabel(QStringLiteral("–"), this);
    m_tapeLabel->setObjectName(QStringLiteral("controlbar.tape.counter"));
    m_tapeLabel->setMinimumWidth(m_tapeLabel->fontMetrics().horizontalAdvance(QStringLiteral("● 00:00 ")));
    layout->addWidget(m_tapeLabel);
    setTapeVisible(false);

    connect(m_modelCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        emit modelSelected(static_cast<Model>(m_modelCombo->itemData(index).toInt()));
    });
    // Buttons are checkable so their own click already toggled the visual
    // check state -- MainWindow will resync it (via setCe150State/
    // setCe1600pState) once PlotterController confirms the actual result,
    // so no manual setChecked() here.
    connect(m_ce150Button, &QPushButton::clicked, this, [this] { emit ce150ToggleRequested(); });
    connect(m_ce1600pButton, &QPushButton::clicked, this, [this] { emit ce1600pToggleRequested(); });
    connect(m_ce158Button, &QPushButton::clicked, this, [this] { emit ce158ToggleRequested(); });
}

void ControlBar::setModel(Model model) {
    const QSignalBlocker blocker(m_modelCombo);
    m_modelCombo->setCurrentIndex(static_cast<int>(model));
}

void ControlBar::setModuleCombos(int slot, const QVector<MemoryModuleManager::ModuleEntry>& templates,
                                  const QVector<MemoryModuleManager::ModuleEntry>& instances,
                                  const QString& selectedOrEmpty) {
    QStringList ram, roms;
    for (const auto& e : templates) (e.rom ? roms : ram) << e.moduleName;
    const auto name = [](const MemoryModuleManager::ModuleEntry& e) { return e.moduleName; };
    fillPicker(m_slot[slot - 1].combo, ram, namesOf(instances, name), selectedOrEmpty, roms);
}

// Enables rather than shows/hides the button, so the control bar doesn't
// shift as modules change.
void ControlBar::setSlotSaveEnabled(int slot, bool enabled) {
    m_slot[slot - 1].saveButton->setEnabled(enabled);
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

void ControlBar::setCe158State(bool attached, bool enabled) {
    const QSignalBlocker blocker(m_ce158Button);
    m_ce158Button->setChecked(attached);
    m_ce158Button->setEnabled(enabled);
}

void ControlBar::setCe1600pState(bool attached, bool enabled) {
    const QSignalBlocker blocker(m_ce1600pButton);
    m_ce1600pButton->setChecked(attached);
    m_ce1600pButton->setEnabled(enabled);
}

void ControlBar::setCe150Visible(bool visible) {
    m_ce150Button->setVisible(visible);
}

void ControlBar::setCe158Visible(bool visible) {
    m_ce158Button->setVisible(visible);
}

void ControlBar::setCe1600pVisible(bool visible) {
    m_ce1600pButton->setVisible(visible);
}

void ControlBar::setFloppyCombo(const QVector<FloppyDiskManager::DiskEntry>& templates,
                                const QVector<FloppyDiskManager::DiskEntry>& instances,
                                const QString& selectedOrEmpty) {
    const auto name = [](const FloppyDiskManager::DiskEntry& e) { return e.diskName; };
    fillPicker(m_floppyCombo, namesOf(templates, name), namesOf(instances, name), selectedOrEmpty);
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
    m_floppySideButton->setEnabled(enabled);
}

void ControlBar::setFloppySaveEnabled(bool enabled) {
    m_floppySaveButton->setEnabled(enabled);
}

void ControlBar::setFloppySide(int side) {
    m_floppySideButton->setText(side ? tr("B") : tr("A"));
}

// Polled every frame tick -- only restyle on an actual change, since
// setStyleSheet() re-polishes the label.
void ControlBar::setFloppyMotorOn(bool on) {
    if (on == m_floppyMotorOn) return;
    m_floppyMotorOn = on;
    applyLampStyle(m_floppyLampLabel, on);
}

// A motor lamp: green while the motor runs, grey otherwise.
void ControlBar::applyLampStyle(QLabel* lamp, bool on) {
    lamp->setStyleSheet(on ? QStringLiteral("color: #2ecc40;") : QStringLiteral("color: #888888;"));
}

void ControlBar::setTapeCombo(const QStringList& names, const QString& selectedOrEmpty) {
    QStringList all = names;
    if (!selectedOrEmpty.isEmpty() && !all.contains(selectedOrEmpty)) all << selectedOrEmpty;
    fillPicker(m_tapeCombo, all, {}, selectedOrEmpty);
}

void ControlBar::setTapeVisible(bool visible) {
    m_tapeSeparator->setVisible(visible);
    m_tapeTitle->setVisible(visible);
    m_tapeCombo->setVisible(visible);
    m_tapeSaveButton->setVisible(visible);
    m_tapeLabel->setVisible(visible);
}

// "▶ 0:12" playing (position on the tape), "● 0:03" recording (recorded so
// far), "–" empty. The file name goes in the tooltip. The counter doubles as
// the motor lamp: green while the remote relay runs the motor.
void ControlBar::setTapeStatus(const TapeDeck::Status& status) {
    QString text = QStringLiteral("–");
    if (status.mode != TapeDeck::Mode::Empty) {
        const int seconds = static_cast<int>(status.position);
        text = QStringLiteral("%1 %2:%3")
                   .arg(status.mode == TapeDeck::Mode::Play ? QStringLiteral("▶") : QStringLiteral("●"))
                   .arg(seconds / 60)
                   .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    }
    if (text != m_tapeLabel->text()) {
        m_tapeLabel->setText(text);
        const QString name = QFileInfo(QString::fromStdString(status.path)).fileName();
        const QString what = status.mode == TapeDeck::Mode::Play     ? tr("Playing %1").arg(name)
                             : status.mode == TapeDeck::Mode::Record ? tr("Recording into %1").arg(name)
                                                                     : tr("No tape");
        m_tapeLabel->setToolTip(tr("%1 (green while the motor runs)").arg(what));
    }
    // Off is the default text color, not the lamps' grey, which would read as disabled.
    if (status.motor != m_tapeMotorOn) {
        m_tapeMotorOn = status.motor;
        m_tapeLabel->setStyleSheet(m_tapeMotorOn ? QStringLiteral("color: #2ecc40;") : QString());
    }
}
