#include "MainWindow.hpp"
#include "FaceplateWidget.hpp"
#include "LcdWidget.hpp"
#include "ControlBar.hpp"
#include "DebugPanel.hpp"
#include "MachineController.hpp"
#include "MemoryModuleManager.hpp"
#include "PC1500KeyboardMap.hpp"
#include "PlotterController.hpp"
#include "PlotterPaperWidget.hpp"
#include "SettingsDialog.hpp"
#include "AboutDialog.hpp"
#include "PresetController.hpp"
#include "AppSettings.hpp"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTimer>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QFileDialog>
#include <QDir>
#include <QCoreApplication>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QKeySequence>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("Calc-U-1600"));
    setFocusPolicy(Qt::StrongFocus);

    m_controller = std::make_unique<MachineController>(this);
    m_moduleManager = std::make_unique<MemoryModuleManager>(m_controller.get(), this);
    m_controller->setModuleManager(m_moduleManager.get());
    m_presetController = std::make_unique<PresetController>(m_controller.get(), m_moduleManager.get(), this);

    // QMainWindow requires exactly one central widget -- everything that
    // used to be added straight into `this`'s own QVBoxLayout now lives in
    // this wrapper instead, freeing `this` up for setMenuBar() below.
    auto* central = new QWidget(this);
    m_faceplate = new FaceplateWidget(central);
    m_controlBar = new ControlBar(central);
    m_debugPanel = new DebugPanel(m_controller.get(), central);
    m_debugPanel->setModuleManager(m_moduleManager.get());
    m_plotterController = std::make_unique<PlotterController>(m_controller.get(), this);
    m_plotterPaper = new PlotterPaperWidget(m_controller.get(), central);
    m_plotterPaper->hide(); // added to m_debugRowLayout only once a plotter attaches

    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(6, 6, 6, 4);
    layout->setSpacing(4);
    // Faceplate is stretch 0 (pinned to its own aspect-locked heightForWidth
    // size, top-aligned) so extra vertical space goes to the debug row
    // instead of letterboxing the calculator body -- see FaceplateWidget's
    // own heightForWidth()/sizeHint().
    layout->addWidget(m_faceplate, /*stretch=*/0);
    layout->addWidget(m_controlBar, /*stretch=*/0);

    m_debugRow = new QWidget(central);
    m_debugRowLayout = new QHBoxLayout(m_debugRow);
    m_debugRowLayout->setContentsMargins(0, 0, 0, 0);
    m_debugRowLayout->setSpacing(4);
    m_debugRowLayout->addWidget(m_debugPanel, /*stretch=*/2);
    layout->addWidget(m_debugRow, /*stretch=*/1);

    setCentralWidget(central);
    buildMenuBar();

    connect(m_controlBar, &ControlBar::modelSelected, this, &MainWindow::applyModelSelection);
    connect(m_controlBar, &ControlBar::romRevisionSelected, this, &MainWindow::applyRomRevisionSelection);
    connect(m_controlBar, &ControlBar::resetClicked, this, [this](bool allReset) {
        if (allReset) {
            m_controller->resetAll();
        } else {
            m_controller->resetSimple();
        }
    });
    connect(m_controlBar, &ControlBar::moduleSelected, this, [this](int slot, QString moduleNameOrEmpty) {
        m_moduleManager->selectModule(slot, moduleNameOrEmpty);
        m_controller->switchModel(m_controller->currentModel()); // rebuild -> re-attach
        refreshModuleCombos();
    });
    connect(m_controlBar, &ControlBar::nameAndSaveRequested, this, [this](int slot) {
        const bool prefillCurrent = m_moduleManager->slotHasInstanceFile(slot);
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("Name & Save"), tr("Instance name:"), QLineEdit::Normal,
            prefillCurrent ? m_moduleManager->selectedModuleName(slot) : QString(), &ok);
        if (!ok) return;
        QString error;
        if (!m_moduleManager->nameAndSave(slot, name, &error)) {
            QMessageBox::warning(this, tr("Name & Save"), error);
            return;
        }
        refreshModuleCombos();
    });
    auto openSettingsDialog = [this] {
        SettingsDialog dialog(m_controller.get(), this);
        dialog.exec();
    };
    connect(m_controlBar, &ControlBar::settingsRequested, this, openSettingsDialog);
    connect(m_presetController.get(), &PresetController::armed, this, &MainWindow::onPresetArmed);
    auto openPresetDialog = [this] {
        const QString startDir = AppSettings::presetOpenDir().isEmpty() ? QDir::homePath()
                                                                          : AppSettings::presetOpenDir();
        const QString path = QFileDialog::getOpenFileName(this, tr("Load Preset"), startDir,
                                                            tr("Presets (*.pc1500 *.pc1500a *.pc1600);;All Files (*)"));
        if (path.isEmpty()) return;

        // A preset doesn't just open a file -- it replaces the running
        // machine (model/ROM/modules/plotter/program) with whatever the
        // preset specifies, same destructive-to-current-state rationale as
        // Load BASIC Program's own confirmation.
        const auto reply = QMessageBox::question(
            this, tr("Load Preset"),
            tr("This replaces the current machine and its state with the one specified in the preset. Continue?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (reply != QMessageBox::Yes) return;

        // Stop the frame timer for the (possibly multi-second, synchronous)
        // duration of the load -- see PresetController's own doc comment
        // for why: nothing else may drive the machine while the preset
        // loader is mid-script.
        m_frameTimer->stop();
        m_moduleManager->flushPendingPersist();
        setCursor(Qt::WaitCursor);
        QString error;
        const bool ok = m_presetController->loadPreset(path, &error);
        unsetCursor();

        // PresetController::armed (connected above to onPresetArmed()) has
        // already resynced the control bar/plotter/module combos once,
        // mid-load, while the machine was still armed-but-off. Call it
        // again now that loadPreset() has returned so a preset that failed
        // before ever arming (bad modulespec, missing ROM, ...) -- which
        // never fires armed() -- still gets the UI resynced to whatever's
        // actually attached; resetBareForPresetPC1600/1500() already
        // replaced the underlying machine either way.
        onPresetArmed();
        m_frameTimer->start(16);

        if (!ok) {
            QMessageBox::warning(this, tr("Load Preset"), error);
        }
    };
    connect(m_controlBar, &ControlBar::openPresetRequested, this, openPresetDialog);
    // File/Help menu actions reuse the exact same handlers as their
    // ControlBar equivalents -- see buildMenuBar()'s own doc comment.
    connect(m_openPresetAction, &QAction::triggered, this, openPresetDialog);
    connect(m_settingsAction, &QAction::triggered, this, openSettingsDialog);
    connect(m_aboutAction, &QAction::triggered, this, [this] {
        AboutDialog dialog(this);
        dialog.exec();
    });
    connect(m_loadBasicProgramAction, &QAction::triggered, this, [this] {
        // Shares the "Default samples folder" setting with Load Preset --
        // both preset files and bare .bas listings live in the same
        // samples folder in practice, so one setting covers both pickers.
        const QString startDir = AppSettings::presetOpenDir().isEmpty() ? QDir::homePath()
                                                                          : AppSettings::presetOpenDir();
        const QString path = QFileDialog::getOpenFileName(this, tr("Load BASIC Program"), startDir,
                                                            tr("BASIC Programs (*.bas);;All Files (*)"));
        if (path.isEmpty()) return;

        // Step 0: tokenize before touching anything else -- a listing that
        // doesn't even tokenize should fail here with no popup and no
        // reset, not after the machine's already been cleared for it.
        QString precheckError;
        if (!m_presetController->checkBasicProgramTokenizes(path, &precheckError)) {
            QMessageBox::warning(this, tr("Load BASIC Program"), precheckError);
            return;
        }

        // The load choreography resets the machine and clears the resident
        // program (see PresetController::loadBasicProgramLive) -- unlike
        // Load Preset, this runs against a machine the user may have been
        // actively using, so confirm first.
        const auto reply = QMessageBox::question(
            this, tr("Load BASIC Program"),
            tr("This resets the machine and clears the current program before loading the new one. Continue?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (reply != QMessageBox::Yes) return;

        // Same rationale as Load Preset -- stop the frame timer for the
        // duration of the (synchronous) load so nothing else drives the
        // machine mid-script.
        m_frameTimer->stop();
        m_moduleManager->flushPendingPersist();
        setCursor(Qt::WaitCursor);
        QString error;
        const bool ok = m_presetController->loadBasicProgramLive(path, &error);
        unsetCursor();
        m_frameTimer->start(16);

        if (!ok) {
            QMessageBox::warning(this, tr("Load BASIC Program"), error);
        }
    });
    connect(m_moduleManager.get(), &MemoryModuleManager::errorMessage, this,
            [this](const QString& text) { QMessageBox::warning(this, tr("Memory Module"), text); });
    connect(m_controlBar, &ControlBar::ce150ToggleRequested, this,
            [this] { m_plotterController->requestToggleCE150(); });
    connect(m_controlBar, &ControlBar::ce1600pToggleRequested, this,
            [this] { m_plotterController->requestToggleCE1600P(); });
    connect(m_plotterController.get(), &PlotterController::busyChanged, this, [this](bool busy) {
        m_controlBar->setCe150State(m_controller->ce150Attached(), !busy && !m_controller->ce1600pAttached());
        m_controlBar->setCe1600pState(m_controller->ce1600pAttached(), !busy && !m_controller->ce150Attached());
    });
    connect(m_plotterController.get(), &PlotterController::ce150AttachedChanged, this,
            [this](bool attached) { onPlotterAttachedChanged(/*isCE150=*/true, attached); });
    connect(m_plotterController.get(), &PlotterController::ce1600pAttachedChanged, this,
            [this](bool attached) { onPlotterAttachedChanged(/*isCE150=*/false, attached); });
    connect(m_faceplate, &FaceplateWidget::keyPressed, this, [this](QString name) {
        const std::string key = name.toStdString();
        if (key == "on") {
            m_controller->setOnKeyPressed(true);
        } else {
            m_controller->pressKey(key);
        }
    });
    connect(m_faceplate, &FaceplateWidget::keyReleased, this, [this](QString name) {
        const std::string key = name.toStdString();
        if (key == "on") {
            m_controller->setOnKeyPressed(false);
        } else {
            m_controller->releaseKey(key);
        }
    });

    m_frameTimer = new QTimer(this);
    m_frameTimer->setTimerType(Qt::PreciseTimer);
    connect(m_frameTimer, &QTimer::timeout, this, &MainWindow::onFrameTick);
    m_frameTimer->start(16); // ~60 Hz

    // MachineController's constructor already booted whatever model the
    // "Startup device" setting picked (see AppSettings::startupModelPreference()),
    // but the faceplate/control bar were built with their own hardcoded
    // PC-1500A defaults -- resync them now so the visible calculator
    // actually matches what's running underneath.
    m_faceplate->setModel(m_controller->currentModel());
    m_controlBar->setModel(m_controller->currentModel());
    m_controlBar->setRomRevision(m_controller->pc1500RomRevision());
    syncControlBarForModel();
    syncMachineMenuFromModel(m_controller->currentModel());
    syncMachineMenuFromRomRevision(m_controller->pc1500RomRevision());
    refreshModuleCombos();

    resize(AppSettings::windowSize());
    setFocus();
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* event) {
    AppSettings::setWindowSize(size());
    m_moduleManager->flushPendingPersist();
    QMainWindow::closeEvent(event);
}

void MainWindow::syncControlBarForModel() {
    const bool isPC1600 = m_controller->currentModel() == Model::PC1600;
    m_controlBar->setSlot2Visible(isPC1600);
    m_controlBar->setCe1600pVisible(isPC1600);
    // PC-1500A is A04-only (PC1500Variant.hpp), so the picker is only worth
    // showing for the plain PC-1500.
    const bool romPickerVisible = m_controller->currentModel() == Model::PC1500;
    m_controlBar->setRomPickerVisible(romPickerVisible);
    if (m_romMenuAction) m_romMenuAction->setVisible(romPickerVisible);
}

void MainWindow::onPlotterAttachedChanged(bool isCE150, bool attached) {
    const bool ce150Attached = isCE150 ? attached : m_controller->ce150Attached();
    const bool ce1600pAttached = isCE150 ? m_controller->ce1600pAttached() : attached;
    m_controlBar->setCe150State(ce150Attached, !ce1600pAttached);
    m_controlBar->setCe1600pState(ce1600pAttached, !ce150Attached);
    const bool otherAttached = isCE150 ? ce1600pAttached : ce150Attached;
    if (attached) {
        m_plotterPaper->setKind(isCE150 ? PlotterPaperWidget::Kind::CE150 : PlotterPaperWidget::Kind::CE1600P);
        if (!m_plotterPaperInLayout) { m_debugRowLayout->addWidget(m_plotterPaper, 1); m_plotterPaperInLayout = true; }
        m_plotterPaper->show();
    } else if (!otherAttached && m_plotterPaperInLayout) {
        m_debugRowLayout->removeWidget(m_plotterPaper);
        m_plotterPaper->hide();
        m_plotterPaperInLayout = false;
    }
}

void MainWindow::onPresetArmed() {
    m_faceplate->setModel(m_controller->currentModel());
    m_controlBar->setModel(m_controller->currentModel());
    m_controlBar->setRomRevision(m_controller->pc1500RomRevision());
    syncControlBarForModel();
    syncMachineMenuFromModel(m_controller->currentModel());
    syncMachineMenuFromRomRevision(m_controller->pc1500RomRevision());
    refreshModuleCombos();
    // The preset attached its plotter directly on the Core machine,
    // bypassing PlotterController/attachCE150()/attachCE1600P() entirely --
    // resync from the machine's actual state rather than assuming detached.
    m_plotterController->syncFromMachineState();
    // Force a repaint of the armed-but-off state now, before control
    // returns into Core's (possibly many-seconds-long) boot + preset
    // script -- otherwise nothing would reach the screen until the whole
    // preset had run. The setModel()/refreshModuleCombos() calls above only
    // *request* a repaint (QWidget::update(), queued); repaint() forces an
    // immediate, synchronous one, and processEvents() additionally drains
    // any other pending GUI event so the window is fully up to date before
    // this call returns into the blocking preset script below.
    repaint();
    QCoreApplication::processEvents();
}

void MainWindow::refreshModuleCombos() {
    for (int slot = 1; slot <= 2; ++slot) {
        const CardHost host = MemoryModuleManager::hostForModel(slot, m_controller->currentModel());
        const auto bundled = m_moduleManager->bundledEntries(host);
        const auto instance = m_moduleManager->instanceEntries(host);
        m_controlBar->setModuleCombos(slot, bundled, instance, m_moduleManager->selectedModuleName(slot));
        m_controlBar->setSlotBatteryBacked(slot, m_moduleManager->isSlotBatteryBacked(slot, bundled, instance));
    }
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        // Qt delivers OS auto-repeat as repeated .down events -- without
        // this, holding a shift-needing key would re-fire the whole
        // tapShiftedKey choreography on every repeat tick.
        event->accept();
        return;
    }

    const bool isPC1600 = m_controller->currentModel() == Model::PC1600;
    auto resolved = PC1500KeyboardMap::resolve(static_cast<Qt::Key>(event->key()),
                                                event->modifiers(), event->text(), isPC1600);
    if (!resolved) {
        QWidget::keyPressEvent(event);
        return;
    }

    if (resolved->needsShift) {
        // Self-contained fire-and-forget sequence -- nothing to track for
        // the matching .up event, so release skips it too.
        m_controller->tapShiftedKey(resolved->baseKey);
    } else {
        m_controller->pressKey(resolved->baseKey);
        m_physicalKeysDown.insert(event->key(), resolved->baseKey);
    }
    event->accept();
}

void MainWindow::keyReleaseEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }

    auto it = m_physicalKeysDown.find(event->key());
    if (it == m_physicalKeysDown.end()) {
        QWidget::keyReleaseEvent(event);
        return;
    }
    m_controller->releaseKey(it.value());
    m_physicalKeysDown.erase(it);
    event->accept();
}

void MainWindow::onFrameTick() {
    const std::uint64_t cyclesPerFrame = static_cast<std::uint64_t>(m_controller->clockHz() / 60.0);
    m_controller->advance(cyclesPerFrame);

    const DisplayFrame frame = m_controller->currentDisplay();
    m_faceplate->lcdWidget()->setFrame(frame);
    m_faceplate->lcdWidget()->update();

    m_moduleManager->markDirtyAndSchedulePersist();
    m_debugPanel->onFrameTick();
    m_plotterController->onFrameTick();
    if (m_plotterPaperInLayout) m_plotterPaper->onFrameTick();
}

void MainWindow::buildMenuBar() {
    // File: one-shot actions ControlBar's own buttons also expose --
    // openPresetAction/settingsAction/aboutAction are connected by the
    // constructor, right alongside the ControlBar signal they duplicate, so
    // both use the exact same handler closure.
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    m_openPresetAction = fileMenu->addAction(tr("Load Preset…"));
    m_loadBasicProgramAction = fileMenu->addAction(tr("Load BASIC Program…"));
    fileMenu->addSeparator();
    m_settingsAction = fileMenu->addAction(tr("Settings…"));
    fileMenu->addSeparator();
    QAction* quitAction = fileMenu->addAction(tr("Quit"));
    quitAction->setMenuRole(QAction::QuitRole);
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    // Machine: duplicates ControlBar's model/ROM pickers (checkable, exclusive
    // per group) plus Reset/Reset All, which today only reachable via
    // ControlBar's Reset button and its Cmd-click "reset all" gesture.
    QMenu* machineMenu = menuBar()->addMenu(tr("&Machine"));

    QMenu* modelMenu = machineMenu->addMenu(tr("Model"));
    m_modelActionGroup = new QActionGroup(this);
    m_modelActionGroup->setExclusive(true);
    auto addModelAction = [&](Model model, const QString& label) {
        QAction* action = modelMenu->addAction(label);
        action->setCheckable(true);
        m_modelActionGroup->addAction(action);
        m_modelActions.insert(model, action);
        connect(action, &QAction::triggered, this, [this, model] { applyModelSelection(model); });
    };
    addModelAction(Model::PC1500, tr("PC-1500"));
    addModelAction(Model::PC1500A, tr("PC-1500A"));
    addModelAction(Model::PC1600, tr("PC-1600"));

    QMenu* romMenu = machineMenu->addMenu(tr("ROM Revision"));
    m_romMenuAction = romMenu->menuAction();
    m_romActionGroup = new QActionGroup(this);
    m_romActionGroup->setExclusive(true);
    auto addRomAction = [&](PC1500RomRevision revision, const QString& label) {
        QAction* action = romMenu->addAction(label);
        action->setCheckable(true);
        m_romActionGroup->addAction(action);
        m_romActions.insert(revision, action);
        connect(action, &QAction::triggered, this, [this, revision] { applyRomRevisionSelection(revision); });
    };
    addRomAction(PC1500RomRevision::A01, tr("A01"));
    addRomAction(PC1500RomRevision::A03, tr("A03"));
    addRomAction(PC1500RomRevision::A04, tr("A04"));

    machineMenu->addSeparator();
    QAction* resetAction = machineMenu->addAction(tr("Reset"));
    resetAction->setShortcut(QKeySequence(Qt::ControlModifier | Qt::Key_R));
    connect(resetAction, &QAction::triggered, this, [this] { m_controller->resetSimple(); });
    QAction* resetAllAction = machineMenu->addAction(tr("Reset All"));
    resetAllAction->setShortcut(QKeySequence(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_R));
    connect(resetAllAction, &QAction::triggered, this, [this] { m_controller->resetAll(); });

    // Help
    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    m_aboutAction = helpMenu->addAction(tr("About Calc-U-1600…"));
    m_aboutAction->setMenuRole(QAction::AboutRole);
}

void MainWindow::applyModelSelection(Model model) {
    m_moduleManager->flushPendingPersist();
    m_moduleManager->onModelChanged(model);
    m_controller->switchModel(model); // rebuilds the machine -- any live plotter attachment is already gone
    m_plotterController->resetOnModelSwitch();
    m_faceplate->setModel(model);
    m_controlBar->setModel(model);
    m_controlBar->setRomRevision(m_controller->pc1500RomRevision());
    syncControlBarForModel();
    syncMachineMenuFromModel(model);
    syncMachineMenuFromRomRevision(m_controller->pc1500RomRevision());
    refreshModuleCombos();
}

void MainWindow::applyRomRevisionSelection(PC1500RomRevision revision) {
    m_moduleManager->flushPendingPersist();
    m_controller->setPC1500RomRevision(revision); // rebuilds the machine
    m_controlBar->setRomRevision(revision);
    syncMachineMenuFromRomRevision(revision);
    refreshModuleCombos();
}

void MainWindow::syncMachineMenuFromModel(Model model) {
    if (QAction* action = m_modelActions.value(model, nullptr)) action->setChecked(true);
}

void MainWindow::syncMachineMenuFromRomRevision(PC1500RomRevision revision) {
    if (QAction* action = m_romActions.value(revision, nullptr)) action->setChecked(true);
}
