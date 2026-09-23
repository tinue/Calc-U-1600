#include "SettingsDialog.hpp"
#include "AppPaths.hpp"
#include "AppSettings.hpp"
#include "MachineController.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <vector>

namespace {

// Wide enough that typical absolute paths show in full on one line.
constexpr int kMinimumDialogWidth = 960;

// Grid columns shared by every section: label | value (stretches) | pick
// button | reset/clear button.
enum Column { kLabelColumn = 0, kValueColumn = 1, kPickColumn = 2, kResetColumn = 3 };

// Collects every section's grid and row label so the label column can be
// given one common width at the end (see alignLabelColumns()) -- without
// that, each group box sizes its own label column and the value fields
// start at a different x in every section.
struct SectionGrids {
    std::vector<QGridLayout*> grids;
    std::vector<QLabel*> labels;
};

QGridLayout* addSection(QVBoxLayout* layout, QWidget* parent, SectionGrids& sections, const QString& title) {
    auto* box = new QGroupBox(title, parent);
    auto* grid = new QGridLayout(box);
    grid->setColumnStretch(kValueColumn, 1);
    layout->addWidget(box);
    sections.grids.push_back(grid);
    return grid;
}

QLabel* addRowLabel(QGridLayout* grid, int row, QWidget* parent, SectionGrids& sections, const QString& text) {
    auto* label = new QLabel(text, parent);
    grid->addWidget(label, row, kLabelColumn, Qt::AlignRight | Qt::AlignVCenter);
    sections.labels.push_back(label);
    return label;
}

void alignLabelColumns(const SectionGrids& sections) {
    int width = 0;
    for (QLabel* label : sections.labels) width = std::max(width, label->sizeHint().width());
    for (QGridLayout* grid : sections.grids) grid->setColumnMinimumWidth(kLabelColumn, width);
}

QPushButton* makeRowButton(const QString& text, QWidget* parent) {
    auto* button = new QPushButton(text, parent);
    // Without this, Qt/macOS auto-picks the first autoDefault push button
    // added to the dialog as its Return-triggered "default" button and
    // renders it blue -- misleading here, since no row's buttons are more
    // "primary" than any other's (and Close should be the default).
    button->setAutoDefault(false);
    return button;
}

// Everything one path row needs. `display` returns the effective path to
// show (empty = show `placeholder` instead); `isOverridden` says whether a
// user value is stored, which is what enables the reset/clear button.
// `pick` runs the picker dialog and returns the chosen path (empty =
// cancelled); `set` writes through to AppSettings (empty = back to
// default); `onChanged` runs after either button, for a row that needs to
// do more than refresh itself (e.g. relinking a live serial port).
struct PathRowSpec {
    QString label;
    QString pickText = SettingsDialog::tr("Change…");
    QString resetText = SettingsDialog::tr("Reset");
    QString resetToolTip = SettingsDialog::tr("Revert to the default location");
    QString placeholder;
    std::function<QString()> pick;
    std::function<QString()> display;
    std::function<bool()> isOverridden;
    std::function<void(const QString&)> set;
    std::function<void()> onChanged;
};

// One row: right-aligned label, read-only single-line path field (scrolls
// horizontally, selectable, full path in the tooltip), pick + reset buttons.
void addPathRow(QGridLayout* grid, int row, QWidget* parent, SectionGrids& sections, const PathRowSpec& spec) {
    addRowLabel(grid, row, parent, sections, spec.label);

    auto* field = new QLineEdit(parent);
    field->setReadOnly(true);
    field->setPlaceholderText(spec.placeholder);
    grid->addWidget(field, row, kValueColumn);

    auto* pickButton = makeRowButton(spec.pickText, parent);
    auto* resetButton = makeRowButton(spec.resetText, parent);
    resetButton->setToolTip(spec.resetToolTip);
    grid->addWidget(pickButton, row, kPickColumn);
    grid->addWidget(resetButton, row, kResetColumn);

    auto refresh = [field, resetButton, spec] {
        const QString text = spec.display();
        field->setText(text);
        field->setCursorPosition(0);
        field->setToolTip(text);
        resetButton->setEnabled(spec.isOverridden());
    };
    refresh();

    QObject::connect(pickButton, &QPushButton::clicked, parent, [spec, refresh] {
        const QString chosen = spec.pick();
        if (chosen.isEmpty()) return;
        spec.set(chosen);
        refresh();
        if (spec.onChanged) spec.onChanged();
    });
    QObject::connect(resetButton, &QPushButton::clicked, parent, [spec, refresh] {
        spec.set(QString());
        refresh();
        if (spec.onChanged) spec.onChanged();
    });
}

// A directory row: the picker opens on `startDir`.
PathRowSpec directorySpec(QWidget* parent, const QString& label, const QString& dialogTitle,
                          const std::function<QString()>& startDir) {
    PathRowSpec spec;
    spec.label = label;
    spec.pick = [parent, dialogTitle, startDir] {
        return QFileDialog::getExistingDirectory(parent, dialogTitle, startDir());
    };
    return spec;
}

// One file-open start folder row (AppSettings::OpenFolder). Unset -- the
// default, and what Reset restores -- shows "<last used>": that dialog
// starts wherever a file was last picked from.
void addOpenFolderRow(QGridLayout* grid, int row, QWidget* parent, SectionGrids& sections, const QString& label,
                      const QString& dialogTitle, AppSettings::OpenFolder folder) {
    PathRowSpec spec = directorySpec(parent, label, dialogTitle, [folder] { return AppSettings::openStartDir(folder); });
    spec.resetToolTip = SettingsDialog::tr("Start in the folder a file was last loaded from");
    spec.display = [folder] {
        const QString dir = AppSettings::openDir(folder);
        return dir.isEmpty() ? SettingsDialog::tr("<last used>") : dir;
    };
    spec.isOverridden = [folder] { return !AppSettings::openDir(folder).isEmpty(); };
    spec.set = [folder](const QString& dir) { AppSettings::setOpenDir(folder, dir); };
    addPathRow(grid, row, parent, sections, spec);
}

// One model's "default preset" row: `modelKey` is AppSettings::
// defaultPresetPath()'s key, `extension` the preset suffix that model uses.
void addDefaultPresetRow(QGridLayout* grid, int row, QWidget* parent, SectionGrids& sections, const QString& label,
                         Model model) {
    const QString modelKey = modelSettingsKey(model);
    const QString extension = modelKey.toLower();
    PathRowSpec spec;
    spec.label = label;
    spec.pickText = SettingsDialog::tr("Choose…");
    spec.resetText = SettingsDialog::tr("Clear");
    spec.resetToolTip = SettingsDialog::tr("Don't load a preset for this model");
    spec.placeholder = SettingsDialog::tr("(none)");
    spec.pick = [parent, modelKey, extension] {
        const QString current = AppSettings::defaultPresetPath(modelKey);
        return QFileDialog::getOpenFileName(parent, SettingsDialog::tr("Choose Default Preset"),
                                            current.isEmpty() ? AppSettings::openStartDir(AppSettings::OpenFolder::Samples) : current,
                                            SettingsDialog::tr("Presets (*.%1);;All Files (*)").arg(extension));
    };
    spec.display = [modelKey] { return AppSettings::defaultPresetPath(modelKey); };
    spec.isOverridden = [modelKey] { return !AppSettings::defaultPresetPath(modelKey).isEmpty(); };
    spec.set = [modelKey](const QString& path) { AppSettings::setDefaultPresetPath(modelKey, path); };
    addPathRow(grid, row, parent, sections, spec);
}

} // namespace

SettingsDialog::SettingsDialog(MachineController* controller, QWidget* parent)
    : QDialog(parent), m_controller(controller) {
    setWindowTitle(tr("Settings"));
    setMinimumWidth(kMinimumDialogWidth);

    auto* layout = new QVBoxLayout(this);
    SectionGrids sections;

    // ── General ──────────────────────────────────────────────────────────
    QGridLayout* general = addSection(layout, this, sections, tr("General"));
    addRowLabel(general, 0, this, sections, tr("Startup device:"));
    auto* startupModelCombo = new QComboBox(this);
    // Data strings match AppSettings::startupModelPreference()'s stored
    // values directly -- "last" reuses whatever model was last switched to;
    // the plain PC-1500 always boots on ROM A04 regardless (not settable
    // here -- see AppSettings.hpp's own comment on this).
    startupModelCombo->addItem(tr("Last used"), QStringLiteral("last"));
    startupModelCombo->addItem(tr("PC-1500 (ROM A04)"), QStringLiteral("PC1500"));
    startupModelCombo->addItem(tr("PC-1500A"), QStringLiteral("PC1500A"));
    startupModelCombo->addItem(tr("PC-1600"), QStringLiteral("PC1600"));
    const int startupIdx = startupModelCombo->findData(AppSettings::startupModelPreference());
    startupModelCombo->setCurrentIndex(startupIdx >= 0 ? startupIdx : 0);
    general->addWidget(startupModelCombo, 0, kValueColumn, Qt::AlignLeft);
    connect(startupModelCombo, &QComboBox::currentIndexChanged, this, [startupModelCombo](int index) {
        AppSettings::setStartupModelPreference(startupModelCombo->itemData(index).toString());
    });

    addOpenFolderRow(general, 1, this, sections, tr("Samples folder:"), tr("Choose Samples Folder"),
                     AppSettings::OpenFolder::Samples);
    addOpenFolderRow(general, 2, this, sections, tr("Basic folder:"), tr("Choose Basic Folder"),
                     AppSettings::OpenFolder::Basic);
    addOpenFolderRow(general, 3, this, sections, tr("Assembly folder:"), tr("Choose Assembly Folder"),
                     AppSettings::OpenFolder::Assembly);

    // ── Default presets ──────────────────────────────────────────────────
    // Applied whenever that model gets selected (including at startup) --
    // see MainWindow::applyDefaultPreset().
    QGridLayout* presets =
        addSection(layout, this, sections, tr("Default presets (loaded when the model is selected)"));
    addDefaultPresetRow(presets, 0, this, sections, tr("PC-1500:"), Model::PC1500);
    addDefaultPresetRow(presets, 1, this, sections, tr("PC-1500A:"), Model::PC1500A);
    addDefaultPresetRow(presets, 2, this, sections, tr("PC-1600:"), Model::PC1600);

    // ── Storage ──────────────────────────────────────────────────────────
    QGridLayout* storage = addSection(layout, this, sections, tr("Storage"));
    {
        PathRowSpec spec = directorySpec(this, tr("Battery-card saves:"), tr("Choose Save Directory"),
                                         [] { return AppPaths::instanceDir(); });
        spec.display = [] { return AppPaths::instanceDir(); };
        spec.isOverridden = [] { return !AppSettings::instanceDirOverride().isEmpty(); };
        spec.set = [](const QString& dir) { AppSettings::setInstanceDirOverride(dir); };
        addPathRow(storage, 0, this, sections, spec);
    }

    // ── Tracing ──────────────────────────────────────────────────────────
    QGridLayout* tracing = addSection(layout, this, sections, tr("Tracing"));
    {
        PathRowSpec spec = directorySpec(this, tr("Trace directory:"), tr("Choose Trace Directory"),
                                         [] { return AppPaths::instanceDir(); });
        spec.display = [] {
            const QString dir = AppSettings::traceDirOverride();
            return dir.isEmpty() ? AppPaths::instanceDir() : dir;
        };
        spec.isOverridden = [] { return !AppSettings::traceDirOverride().isEmpty(); };
        spec.set = [](const QString& dir) { AppSettings::setTraceDirOverride(dir); };
        addPathRow(tracing, 0, this, sections, spec);
    }
    addRowLabel(tracing, 1, this, sections, tr("Maximum file size:"));
    auto* traceSizeSpin = new QSpinBox(this);
    traceSizeSpin->setRange(1, 10000);
    traceSizeSpin->setSuffix(tr(" MB"));
    traceSizeSpin->setValue(AppSettings::traceMaxFileSizeMB());
    tracing->addWidget(traceSizeSpin, 1, kValueColumn, Qt::AlignLeft);
    connect(traceSizeSpin, &QSpinBox::valueChanged, this, [](int mb) { AppSettings::setTraceMaxFileSizeMB(mb); });

#ifndef Q_OS_WIN
    // ── Serial port ──────────────────────────────────────────────────────
    // PtySerialLink is POSIX-only (macOS/Linux); on Windows it's an inert
    // stub, so this section is compiled out entirely rather than shown
    // disabled -- there is nothing for it to do there yet.
    // One folder for every emulated port: the PC-1600's own
    // (calcu1600.serial) and the CE-158's (calcu1600-ce158.serial).
    QGridLayout* serial = addSection(layout, this, sections, tr("Serial ports"));
    auto* serialStatusLabel = new QLabel(this);
    serialStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* ce158StatusLabel = new QLabel(this);
    ce158StatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto refreshSerialStatusLabel = [this, serialStatusLabel, ce158StatusLabel] {
        const QString status = m_controller ? m_controller->serialLinkStatus() : QString();
        serialStatusLabel->setText(status.isEmpty() ? tr("(PC-1600 not active)") : status);
        const QString ce158 = m_controller ? m_controller->ce158SerialLinkStatus() : QString();
        ce158StatusLabel->setText(ce158.isEmpty() ? tr("(CE-158 not attached)") : ce158);
    };
    {
        PathRowSpec spec = directorySpec(this, tr("Symlink directory:"), tr("Choose Serial Port Directory"),
                                         [] { return AppPaths::instanceDir(); });
        spec.display = [] {
            const QString dir = AppSettings::serialLinkDirOverride();
            return dir.isEmpty() ? AppPaths::instanceDir() : dir;
        };
        spec.isOverridden = [] { return !AppSettings::serialLinkDirOverride().isEmpty(); };
        spec.set = [](const QString& dir) { AppSettings::setSerialLinkDirOverride(dir); };
        spec.onChanged = [this, refreshSerialStatusLabel] {
            if (m_controller) m_controller->refreshSerialLinkDirectory();
            refreshSerialStatusLabel();
        };
        addPathRow(serial, 0, this, sections, spec);
    }
    addRowLabel(serial, 1, this, sections, tr("PC-1600 port:"));
    serial->addWidget(serialStatusLabel, 1, kValueColumn, 1, 3);
    addRowLabel(serial, 2, this, sections, tr("CE-158 port:"));
    serial->addWidget(ce158StatusLabel, 2, kValueColumn, 1, 3);
    refreshSerialStatusLabel();
#endif

    alignLabelColumns(sections);

    layout->addStretch(1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}
