#include "SettingsDialog.hpp"
#include "AppPaths.hpp"
#include "AppSettings.hpp"
#include "MachineController.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <functional>

namespace {

// A thin horizontal rule between settings sections -- with every row's
// label, value line, and buttons stacked in the same QVBoxLayout with no
// grouping of their own, adjacent sections otherwise read as one
// undifferentiated block.
void addSeparator(QVBoxLayout* layout, QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line);
}

// One "<label>:" row plus a value line and a "Change…"/"Reset to Default"
// button pair that writes through `set` and re-renders the value line via
// `display` (the effective, already-defaulted text to show; called once up
// front and again after each button). `pick` runs the picker dialog and
// returns the chosen path (empty = cancelled); `onChanged` runs after
// either button, for a row that needs to do more than just refresh its own
// label (e.g. relinking a live serial port).
void addPathRow(QVBoxLayout* layout, QWidget* parent, const QString& labelText,
                const std::function<QString()>& pick, const std::function<QString()>& display,
                const std::function<void(const QString&)>& set, const std::function<void()>& onChanged = {}) {
    layout->addWidget(new QLabel(labelText, parent));

    auto* valueLabel = new QLabel(parent);
    valueLabel->setWordWrap(true);
    layout->addWidget(valueLabel);
    valueLabel->setText(display());

    auto* changeButton = new QPushButton(SettingsDialog::tr("Change…"), parent);
    auto* resetButton = new QPushButton(SettingsDialog::tr("Reset to Default"), parent);
    // Without this, Qt/macOS auto-picks the first autoDefault push button
    // added to the dialog as its Return-triggered "default" button and
    // renders it blue -- a styling that has nothing to do with keyboard
    // focus (Tab moves focus, not this), and is misleading here since no
    // row's Change/Reset is more "primary" than any other.
    changeButton->setAutoDefault(false);
    resetButton->setAutoDefault(false);
    layout->addWidget(changeButton);
    layout->addWidget(resetButton);

    auto refresh = [valueLabel, display] { valueLabel->setText(display()); };
    QObject::connect(changeButton, &QPushButton::clicked, parent, [=] {
        const QString chosen = pick();
        if (!chosen.isEmpty()) {
            set(chosen);
            refresh();
            if (onChanged) onChanged();
        }
    });
    QObject::connect(resetButton, &QPushButton::clicked, parent, [=] {
        set(QString());
        refresh();
        if (onChanged) onChanged();
    });
}

// addPathRow() for a directory; `startDir` supplies the folder the picker
// opens on.
void addDirectoryRow(QVBoxLayout* layout, QWidget* parent, const QString& labelText,
                      const QString& dialogTitle, const std::function<QString()>& startDir,
                      const std::function<QString()>& display,
                      const std::function<void(const QString&)>& set,
                      const std::function<void()>& onChanged = {}) {
    addPathRow(
        layout, parent, labelText,
        [=] { return QFileDialog::getExistingDirectory(parent, dialogTitle, startDir()); }, display, set,
        onChanged);
}

// One model's "default preset" row: `modelKey` is AppSettings::
// defaultPresetPath()'s key, `extension` the preset suffix that model uses.
void addDefaultPresetRow(QVBoxLayout* layout, QWidget* parent, const QString& labelText,
                          const QString& modelKey, const QString& extension) {
    addPathRow(
        layout, parent, labelText,
        [=] {
            const QString current = AppSettings::defaultPresetPath(modelKey);
            return QFileDialog::getOpenFileName(
                parent, SettingsDialog::tr("Choose Default Preset"),
                current.isEmpty() ? AppSettings::presetOpenDirOrHome() : current,
                SettingsDialog::tr("Presets (*.%1);;All Files (*)").arg(extension));
        },
        [=] {
            const QString path = AppSettings::defaultPresetPath(modelKey);
            return path.isEmpty() ? SettingsDialog::tr("(none)") : path;
        },
        [=](const QString& path) { AppSettings::setDefaultPresetPath(modelKey, path); });
}

} // namespace

SettingsDialog::SettingsDialog(MachineController* controller, QWidget* parent)
    : QDialog(parent), m_controller(controller) {
    setWindowTitle(tr("Settings"));

    auto* layout = new QVBoxLayout(this);

    auto* startupModelRow = new QWidget(this);
    auto* startupModelLayout = new QHBoxLayout(startupModelRow);
    startupModelLayout->setContentsMargins(0, 0, 0, 0);
    startupModelLayout->addWidget(new QLabel(tr("Startup device:"), startupModelRow));
    auto* startupModelCombo = new QComboBox(startupModelRow);
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
    startupModelLayout->addWidget(startupModelCombo);
    startupModelLayout->addStretch(1);
    layout->addWidget(startupModelRow);
    connect(startupModelCombo, &QComboBox::currentIndexChanged, this, [startupModelCombo](int index) {
        AppSettings::setStartupModelPreference(startupModelCombo->itemData(index).toString());
    });

    addSeparator(layout, this);
    addDirectoryRow(
        layout, this, tr("Battery-card save directory:"), tr("Choose Save Directory"),
        [] { return AppPaths::instanceDir(); }, [] { return AppPaths::instanceDir(); },
        [](const QString& dir) { AppSettings::setInstanceDirOverride(dir); });

    addSeparator(layout, this);
    addDirectoryRow(
        layout, this, tr("Default samples folder:"), tr("Choose Samples Folder"),
        [] { return AppSettings::presetOpenDirOrHome(); },
        [] {
            const QString dir = AppSettings::presetOpenDir();
            return dir.isEmpty() ? SettingsDialog::tr("(system default)") : dir;
        },
        [](const QString& dir) { AppSettings::setPresetOpenDir(dir); });

    // Applied whenever that model gets selected (including at startup) --
    // see MainWindow::applyDefaultPreset().
    addSeparator(layout, this);
    addDefaultPresetRow(layout, this, tr("PC-1500 default preset:"), QStringLiteral("PC1500"),
                        QStringLiteral("pc1500"));
    addDefaultPresetRow(layout, this, tr("PC-1500A default preset:"), QStringLiteral("PC1500A"),
                        QStringLiteral("pc1500a"));
    addDefaultPresetRow(layout, this, tr("PC-1600 default preset:"), QStringLiteral("PC1600"),
                        QStringLiteral("pc1600"));

    addSeparator(layout, this);
    addDirectoryRow(
        layout, this, tr("Trace file save directory:"), tr("Choose Trace Directory"),
        [] { return AppPaths::instanceDir(); },
        [] {
            const QString dir = AppSettings::traceDirOverride();
            return dir.isEmpty() ? AppPaths::instanceDir() : dir;
        },
        [](const QString& dir) { AppSettings::setTraceDirOverride(dir); });

    addSeparator(layout, this);
    auto* traceSizeRow = new QWidget(this);
    auto* traceSizeLayout = new QHBoxLayout(traceSizeRow);
    traceSizeLayout->setContentsMargins(0, 0, 0, 0);
    traceSizeLayout->addWidget(new QLabel(tr("Maximum trace file size:"), traceSizeRow));
    auto* traceSizeSpin = new QSpinBox(traceSizeRow);
    traceSizeSpin->setRange(1, 10000);
    traceSizeSpin->setSuffix(tr(" MB"));
    traceSizeSpin->setValue(AppSettings::traceMaxFileSizeMB());
    traceSizeLayout->addWidget(traceSizeSpin);
    traceSizeLayout->addStretch(1);
    layout->addWidget(traceSizeRow);
    connect(traceSizeSpin, &QSpinBox::valueChanged, this, [](int mb) { AppSettings::setTraceMaxFileSizeMB(mb); });

#ifndef Q_OS_WIN
    // PtySerialLink is POSIX-only (macOS/Linux); on Windows it's an inert
    // stub, so this section is compiled out entirely rather than shown
    // disabled -- there is nothing for it to do there yet.
    addSeparator(layout, this);
    auto* serialStatusLabel = new QLabel(this);
    auto refreshSerialStatusLabel = [this, serialStatusLabel] {
        const QString status = m_controller ? m_controller->serialLinkStatus() : QString();
        serialStatusLabel->setText(status.isEmpty() ? tr("(PC-1600 not active)")
                                                     : tr("Connect a serial client to: %1").arg(status));
    };

    addDirectoryRow(
        layout, this, tr("Serial port symlink directory:"), tr("Choose Serial Port Directory"),
        [] { return AppPaths::instanceDir(); },
        [] {
            const QString dir = AppSettings::serialLinkDirOverride();
            return dir.isEmpty() ? AppPaths::instanceDir() : dir;
        },
        [](const QString& dir) { AppSettings::setSerialLinkDirOverride(dir); },
        [this, refreshSerialStatusLabel] {
            if (m_controller) m_controller->refreshSerialLinkDirectory();
            refreshSerialStatusLabel();
        });

    serialStatusLabel->setWordWrap(true);
    layout->addWidget(serialStatusLabel);
    refreshSerialStatusLabel();
#endif

    addSeparator(layout, this);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}
