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

MainWindow::MainWindow(QWidget* parent) : QWidget(parent) {
    setWindowTitle(tr("Calc-U-1600"));
    setFocusPolicy(Qt::StrongFocus);

    m_controller = std::make_unique<MachineController>(this);
    m_moduleManager = std::make_unique<MemoryModuleManager>(m_controller.get(), this);
    m_controller->setModuleManager(m_moduleManager.get());
    m_presetController = std::make_unique<PresetController>(m_controller.get(), m_moduleManager.get(), this);
    m_faceplate = new FaceplateWidget(this);
    m_controlBar = new ControlBar(this);
    m_debugPanel = new DebugPanel(m_controller.get(), this);
    m_debugPanel->setModuleManager(m_moduleManager.get());
    m_plotterController = std::make_unique<PlotterController>(m_controller.get(), this);
    m_plotterPaper = new PlotterPaperWidget(m_controller.get(), this);
    m_plotterPaper->hide(); // added to m_debugRowLayout only once a plotter attaches

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 4);
    layout->setSpacing(4);
    // Faceplate is stretch 0 (pinned to its own aspect-locked heightForWidth
    // size, top-aligned) so extra vertical space goes to the debug row
    // instead of letterboxing the calculator body -- see FaceplateWidget's
    // own heightForWidth()/sizeHint().
    layout->addWidget(m_faceplate, /*stretch=*/0);
    layout->addWidget(m_controlBar, /*stretch=*/0);

    m_debugRow = new QWidget(this);
    m_debugRowLayout = new QHBoxLayout(m_debugRow);
    m_debugRowLayout->setContentsMargins(0, 0, 0, 0);
    m_debugRowLayout->setSpacing(4);
    m_debugRowLayout->addWidget(m_debugPanel, /*stretch=*/2);
    layout->addWidget(m_debugRow, /*stretch=*/1);

    connect(m_controlBar, &ControlBar::modelSelected, this, [this](Model model) {
        m_moduleManager->flushPendingPersist();
        m_moduleManager->onModelChanged(model);
        m_controller->switchModel(model); // rebuilds the machine -- any live plotter attachment is already gone
        m_plotterController->resetOnModelSwitch();
        m_faceplate->setModel(model);
        m_controlBar->setRomRevision(m_controller->pc1500RomRevision());
        syncControlBarForModel();
        refreshModuleCombos();
    });
    connect(m_controlBar, &ControlBar::romRevisionSelected, this, [this](PC1500RomRevision revision) {
        m_moduleManager->flushPendingPersist();
        m_controller->setPC1500RomRevision(revision); // rebuilds the machine
        refreshModuleCombos();
    });
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
    connect(m_controlBar, &ControlBar::settingsRequested, this, [this] {
        SettingsDialog dialog(m_controller.get(), this);
        dialog.exec();
    });
    connect(m_presetController.get(), &PresetController::armed, this, &MainWindow::onPresetArmed);
    connect(m_controlBar, &ControlBar::openPresetRequested, this, [this] {
        const QString startDir = AppSettings::presetOpenDir().isEmpty() ? QDir::homePath()
                                                                          : AppSettings::presetOpenDir();
        const QString path = QFileDialog::getOpenFileName(this, tr("Open Preset"), startDir,
                                                            tr("Presets (*.pc1500 *.pc1500a *.pc1600);;All Files (*)"));
        if (path.isEmpty()) return;

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
            QMessageBox::warning(this, tr("Open Preset"), error);
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
    refreshModuleCombos();

    resize(AppSettings::windowSize());
    setFocus();
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* event) {
    AppSettings::setWindowSize(size());
    m_moduleManager->flushPendingPersist();
    QWidget::closeEvent(event);
}

void MainWindow::syncControlBarForModel() {
    const bool isPC1600 = m_controller->currentModel() == Model::PC1600;
    m_controlBar->setSlot2Visible(isPC1600);
    m_controlBar->setCe1600pVisible(isPC1600);
    // PC-1500A is A04-only (PC1500Variant.hpp), so the picker is only worth
    // showing for the plain PC-1500.
    m_controlBar->setRomPickerVisible(m_controller->currentModel() == Model::PC1500);
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
