#include "MainWindow.hpp"
#include "FaceplateWidget.hpp"
#include "LcdWidget.hpp"
#include "ControlBar.hpp"
#include "DebugPanel.hpp"
#include "MachineController.hpp"
#include "MemoryModuleManager.hpp"
#include "FloppyDiskManager.hpp"
#include "PC1500KeyboardMap.hpp"
#include "PlotterController.hpp"
#include "PlotterPaperWidget.hpp"
#include "Ce158PrinterWidget.hpp"
#include "SettingsDialog.hpp"
#include "AboutDialog.hpp"
#include "MachineCodeLoadDialog.hpp"
#include "PresetController.hpp"
#include "AudioOutput.hpp"
#include "EmulationPacer.hpp"
#include "debug/DebugController.hpp"
#include "AppSettings.hpp"
#include "MacClipboardImage.h"
#include "PC1500/PC1500Machine.hpp"
#include "PC1500/PC1500MachineCodeLoader.hpp"
#include "PC1600/PC1600Machine.hpp"
#include "PC1600/PC1600MachineCodeLoader.hpp"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTimer>
#include <QElapsedTimer>
#include <QProgressDialog>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QFile>
#include <QFileDialog>
#include <QApplication>
#include <QCoreApplication>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QKeySequence>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <chrono>
#include <cstring>
#include <functional>

namespace {
// runSynchronousLoad(): how often the blocked UI thread pumps its event
// loop mid-load, and how long a load must run before the "Loading..."
// popup appears (short loads finish without flashing it).
constexpr int kLoadPumpIntervalMs = 30;
constexpr int kLoadPopupDelayMs = 500;

// Longest host-Shift press still counted as a tap (see m_shiftTapArmed).
constexpr qint64 kShiftTapMaxMs = 400;
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("Calc-U-1600"));
    setFocusPolicy(Qt::StrongFocus);

    m_controller = std::make_unique<MachineController>(this);
    m_moduleManager = std::make_unique<MemoryModuleManager>(m_controller.get(), this);
    m_controller->setModuleManager(m_moduleManager.get());
    m_floppyManager = std::make_unique<FloppyDiskManager>(m_controller.get(), this);
    m_controller->setFloppyManager(m_floppyManager.get());
    m_presetController = std::make_unique<PresetController>(m_controller.get(), m_moduleManager.get(),
                                                             m_floppyManager.get(), this);

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
    m_ce158Printer = new Ce158PrinterWidget(m_controller.get(), central);
    m_ce158Printer->hide(); // added to m_debugRowLayout only once a CE-158 attaches
    // Handles screenshot scenarios address these by (docs/screenshots/README.md).
    m_faceplate->setObjectName(QStringLiteral("faceplate"));
    m_faceplate->lcdWidget()->setObjectName(QStringLiteral("lcd"));
    m_debugPanel->setObjectName(QStringLiteral("debugpanel"));
    m_plotterPaper->setObjectName(QStringLiteral("paper"));
    m_ce158Printer->setObjectName(QStringLiteral("ce158printer"));

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
    connect(m_controlBar, &ControlBar::pc1600RomVersionSelected, this, &MainWindow::applyPC1600RomVersionSelection);
    connect(m_controlBar, &ControlBar::ce1600pRomVersionSelected, this, &MainWindow::applyCE1600PRomVersionSelection);
    connect(m_controlBar, &ControlBar::moduleSelected, this, [this](int slot, QString moduleNameOrEmpty) {
        m_moduleManager->selectModule(slot, moduleNameOrEmpty);
        m_controller->switchModel(m_controller->currentModel(), /*keepPlotter=*/true); // rebuild -> re-attach
        m_pacer->restart(); // after the rebuild's flat-out boot
        m_plotterController->syncFromMachineState(); // the plotter survives the rebuild
        refreshModuleCombos();
    });
    connect(m_controlBar, &ControlBar::nameAndSaveRequested, this, [this](int slot) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Name & Save"), tr("Instance name:"),
                                                   QLineEdit::Normal, QString(), &ok);
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
    connect(m_presetController.get(), &PresetController::armed, this, &MainWindow::onPresetArmed);
    auto openPresetDialog = [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Load Preset"),
                                                            AppSettings::openStartDir(AppSettings::OpenFolder::Samples),
                                                            tr("Presets (*.pc1500 *.pc1500a *.pc1600);;All Files (*)"));
        if (path.isEmpty()) return;
        AppSettings::rememberOpenFile(AppSettings::OpenFolder::Samples, path);

        runSynchronousLoad(
            tr("Load Preset"), [this, path](QString* error) { return m_presetController->loadPreset(path, error); },
            // PresetController::armed (connected above to onPresetArmed())
            // has already resynced the control bar/plotter/module combos
            // once, mid-load, while the machine was still armed-but-off.
            // Call it again now that loadPreset() has returned so a preset
            // that failed before ever arming (bad modulespec, missing ROM,
            // ...) -- which never fires armed() -- still gets the UI
            // resynced to whatever's actually attached;
            // resetBareForPresetPC1600/1500() already replaced the
            // underlying machine either way.
            [this] { onPresetArmed(); });
    };
    // Menu actions built in buildMenuBar() -- see its own doc comment.
    connect(m_openPresetAction, &QAction::triggered, this, openPresetDialog);
    connect(m_settingsAction, &QAction::triggered, this, openSettingsDialog);
    connect(m_aboutAction, &QAction::triggered, this, [this] {
        AboutDialog dialog(this);
        dialog.exec();
    });
    connect(m_loadBasicProgramAction, &QAction::triggered, this, [this] {
        // Starts in Settings' "Basic folder" (fixed or <last used>).
        const QString path = QFileDialog::getOpenFileName(this, tr("Load BASIC Program"),
                                                            AppSettings::openStartDir(AppSettings::OpenFolder::Basic),
                                                            tr("BASIC Programs (*.bas);;All Files (*)"));
        if (path.isEmpty()) return;
        AppSettings::rememberOpenFile(AppSettings::OpenFolder::Basic, path);

        runSynchronousLoad(tr("Load BASIC Program"), [this, path](QString* error) {
            return m_presetController->loadBasicProgramLive(path, error);
        });
    });
    connect(m_loadMachineCodeAction, &QAction::triggered, this, &MainWindow::loadMachineCode);
    connect(m_moduleManager.get(), &MemoryModuleManager::errorMessage, this,
            [this](const QString& text) { QMessageBox::warning(this, tr("Memory Module"), text); });
    connect(m_controlBar, &ControlBar::floppyDiskSelected, this, [this](QString diskNameOrEmpty) {
        m_floppyManager->selectDisk(diskNameOrEmpty);
        refreshFloppyCombo();
    });
    connect(m_controlBar, &ControlBar::floppySideToggleRequested, this, [this] {
        m_floppyManager->toggleSide();
        m_controlBar->setFloppySide(m_floppyManager->side());
    });
    connect(m_controlBar, &ControlBar::floppyNameAndSaveRequested, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Name & Save"), tr("Disk name:"),
                                                   QLineEdit::Normal, QString(), &ok);
        if (!ok) return;
        QString error;
        if (!m_floppyManager->nameAndSave(name, &error)) {
            QMessageBox::warning(this, tr("Name & Save"), error);
            return;
        }
        refreshFloppyCombo();
    });
    connect(m_floppyManager.get(), &FloppyDiskManager::errorMessage, this,
            [this](const QString& text) { QMessageBox::warning(this, tr("Floppy Disk"), text); });
    connect(m_controlBar, &ControlBar::ce150ToggleRequested, this,
            [this] { m_plotterController->requestToggleCE150(); });
    connect(m_controlBar, &ControlBar::ce1600pToggleRequested, this,
            [this] { m_plotterController->requestToggleCE1600P(); });
    m_plotterController->setPowerCycleRunner([this](const std::function<void()>& change) {
        // Flat out, like Reset: the pin header's power-on rotation isn't worth
        // watching, and the clock is re-injected afterwards.
        runSynchronousLoad(tr("Plotter"), [this, &change](QString* error) {
            return m_presetController->powerCycleLive(change, error);
        });
    });
    connect(m_plotterController.get(), &PlotterController::attachStateChanged, this, &MainWindow::syncPeripherals);
    connect(m_controlBar, &ControlBar::ce158ToggleRequested, this,
            [this] { m_plotterController->requestToggleCE158(); });
    // Queued: the failure is reported from inside the power cycle, and the
    // dialog should appear once the machine is back on, not block it off.
    connect(
        m_plotterController.get(), &PlotterController::attachFailed, this,
        [this](const QString& device, const QString& reason) {
            QMessageBox::warning(this, tr("Attach %1").arg(device),
                                 reason.isEmpty() ? tr("The %1 could not be attached.").arg(device)
                                                  : tr("The %1 could not be attached:\n\n%2").arg(device, reason));
        },
        Qt::QueuedConnection);
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
    // A key held while focus moves to another widget (a combo box, a
    // dialog) releases there, not here -- let go of it now instead.
    connect(qApp, &QApplication::focusChanged, this, [this] { releaseHeldKeys(); });
    connect(m_faceplate->lcdWidget(), &LcdWidget::turboRequested, this,
            [this](bool active) { m_pacer->setTurbo(active); });

    // The debugger's pause shows in the title bar.
    connect(m_controller->debugController(), &DebugController::pausedChanged, this, [this](bool paused) {
        setWindowTitle(paused ? tr("Calc-U-1600 — Paused (debugger)") : tr("Calc-U-1600"));
    });

    // A debugger's attach `preset:` loads the way File > Load Preset does.
    m_controller->debugController()->setPresetLoader([this](const QString& path, QString* error) {
        return runSynchronousLoad(
            tr("Load Preset"), [this, path](QString* loadError) { return m_presetController->loadPreset(path, loadError); },
            [this] { onPresetArmed(); }, error);
    });

    m_audio = new AudioOutput(this);
    m_pacer = new EmulationPacer(m_controller.get(), m_audio, [this] { refreshViewsAfterAdvance(); }, this);

    // MachineController's constructor already booted whatever model the
    // "Startup device" setting picked (see AppSettings::startupModelPreference()),
    // but the faceplate/control bar were built with their own hardcoded
    // PC-1500A defaults -- resync them now so the visible calculator
    // actually matches what's running underneath.
    syncUiFromController();

    resize(AppSettings::windowSize());
    setFocus();

    // Starting up selects the startup model too -- apply its default preset
    // once the event loop runs, so the window is already up while a preset
    // with long `wait:` steps plays out.
    QTimer::singleShot(0, this, [this] { applyDefaultPreset(m_controller->currentModel()); });
}

MainWindow::~MainWindow() {
    // The child widgets hold raw MachineController pointers, and some use
    // them on destruction (~DebugPanel ends an active TRACE session through
    // the controller). ~QWidget only deletes children after the unique_ptr
    // members -- m_controller included -- are gone, so tear the widget tree
    // down here while the controller still exists.
    m_pacer->suspend();
    delete takeCentralWidget();
    m_faceplate = nullptr;
    m_controlBar = nullptr;
    m_debugPanel = nullptr;
    m_plotterPaper = nullptr;
    m_ce158Printer = nullptr;
    m_debugRow = nullptr;
    m_debugRowLayout = nullptr;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    AppSettings::setWindowSize(size());
    m_moduleManager->flushPendingPersist();
    m_floppyManager->flushPendingPersist();
    QMainWindow::closeEvent(event);
}

void MainWindow::syncControlBarForModel() {
    const bool isPC1600 = m_controller->currentModel() == Model::PC1600;
    m_controlBar->setSlot2Visible(isPC1600);
    m_controlBar->setCe1600pVisible(isPC1600);
    m_controlBar->setCE1600PRomPickerVisible(isPC1600);
    m_ce1600pRomMenuAction->setVisible(isPC1600);
    // Always shown on a PC-1600 (never hidden alongside the CE-1600P
    // toggle) so the control bar doesn't jump around as the plotter/
    // floppy union attaches and detaches -- syncPeripherals()
    // grays the row out instead via setFloppyEnabled().
    m_controlBar->setFloppyVisible(isPC1600);
    if (isPC1600) refreshFloppyCombo();
    // PC-1500A is A04-only (PC1500Variant.hpp), so the picker is only worth
    // showing for the plain PC-1500.
    const bool romPickerVisible = m_controller->currentModel() == Model::PC1500;
    m_controlBar->setRomPickerVisible(romPickerVisible);
    m_romMenuAction->setVisible(romPickerVisible);
    m_controlBar->setPC1600RomPickerVisible(isPC1600);
    m_rom1600MenuAction->setVisible(isPC1600);
}

bool MainWindow::runSynchronousLoad(const QString& title, const std::function<bool(QString*)>& loadFn,
                                    const std::function<void()>& afterLoad, QString* error) {
    // Stop the frame timer for the (possibly multi-second, synchronous)
    // duration of the load -- see PresetController's own doc comment for
    // why: nothing else may drive the machine while a preset/BASIC-program
    // loader is mid-script.
    m_pacer->suspend();
    m_controller->debugController()->setAppBusy(true); // a debugger's requests wait for the load
    m_moduleManager->flushPendingPersist();
    m_floppyManager->flushPendingPersist();
    setCursor(Qt::WaitCursor);
    m_loading = true;

    // The load itself blocks this thread, so pump the event loop from the
    // machine's yield hook (see PresetController::setYieldHook()): keeps the
    // window painting (no beachball) and, once the load has run long enough
    // to be noticeable, shows a "Loading..." popup. User input stays
    // excluded -- nothing may touch the machine until the load returns.
    QElapsedTimer sinceStart;
    QElapsedTimer sincePump;
    sinceStart.start();
    sincePump.start();
    std::unique_ptr<QProgressDialog> popup;
    m_presetController->setYieldHook([&] {
        if (sincePump.elapsed() < kLoadPumpIntervalMs) return;
        sincePump.restart();
        if (!popup && sinceStart.elapsed() >= kLoadPopupDelayMs) {
            popup = std::make_unique<QProgressDialog>(tr("Loading…"), QString(), 0, 0, this);
            popup->setWindowTitle(title);
            popup->setWindowModality(Qt::WindowModal);
            popup->setMinimumDuration(0);
            popup->show();
        }
        // Let the LCD follow along too (the frame timer that normally
        // refreshes it is stopped for the load).
        m_faceplate->lcdWidget()->setFrame(m_controller->currentDisplay());
        m_faceplate->lcdWidget()->update();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    });

    QString loadError;
    const bool ok = loadFn(&loadError);
    m_loading = false;
    m_presetController->setYieldHook({});
    popup.reset();
    unsetCursor();
    if (afterLoad) afterLoad();
    // The load ran the machine flat out -- whatever it beeped is stale.
    m_controller->discardAudio();
    m_pacer->resume();
    m_controller->debugController()->setAppBusy(false);

    if (!ok) {
        if (error) *error = loadError;
        else QMessageBox::warning(this, title, loadError);
    }
    return ok;
}

void MainWindow::resetMachine(bool allReset) {
    // Boot flat out to the prompt (incl. a plotter's power-on init) instead
    // of watching it in real time; the clock is set from the host after.
    runSynchronousLoad(allReset ? tr("Reset All") : tr("Reset"), [this, allReset](QString* error) {
        return m_presetController->resetLive(allReset, error);
    });
}

void MainWindow::loadMachineCode() {
    const QString title = tr("Load Machine Code");
    const QString path = QFileDialog::getOpenFileName(this, title,
                                                      AppSettings::openStartDir(AppSettings::OpenFolder::Assembly),
                                                      tr("Machine Code (*.bin);;All Files (*)"));
    if (path.isEmpty()) return;
    AppSettings::rememberOpenFile(AppSettings::OpenFolder::Assembly, path);

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, title, tr("Could not read %1: %2").arg(path, file.errorString()));
        return;
    }
    const QByteArray raw = file.readAll();
    const machinecode::File code =
        machinecode::readFile(std::vector<uint8_t>(raw.begin(), raw.end()));

    const bool isPC1600 = m_controller->currentModel() == Model::PC1600;
    const machinecode::Target target = isPC1600 ? machinecode::Target::PC1600 : machinecode::Target::PC1500;
    // PC-1600 code always goes into BASIC's program area ("S0"); where that
    // starts (internal RAM or a folded-in RAM module) decides the target.
    std::vector<machinecode::BasicArea> basicAreas;
    if (isPC1600 && m_controller->pc1600()) basicAreas = pc1600BasicAreas(*m_controller->pc1600());
    const machinecode::Plan plan = machinecode::plan(target, code, basicAreas);
    if (!plan.error.empty()) {
        QMessageBox::warning(this, title, QString::fromStdString(plan.error));
        return;
    }

    PresetController::MachineCodeLoadRequest request;
    request.payload = code.payload;
    request.addr = code.loadAddr;
    machinecode::Slot slot = plan.slot;
    if (plan.needsAddress) {
        MachineCodeLoadDialog dialog(this, target, code.payload.size(), plan.defaultAddr, basicAreas);
        if (dialog.exec() != QDialog::Accepted) return;
        request.addr = dialog.address();
        slot = dialog.slot();
    }
    request.slot = static_cast<int>(slot);

    bool loaded = false;
    runSynchronousLoad(title, [this, &request, &loaded](QString* error) {
        loaded = m_presetController->loadMachineCodeLive(request, error);
        return loaded;
    });
    if (!loaded) return;

    // The advice: NEW that keeps BASIC off the code, CALL that starts it.
    uint32_t ramStart = 0, ramEnd = 0;
    if (!isPC1600) {
        if (PC1500Machine* pc1500 = m_controller->pc1500()) {
            ramStart = static_cast<uint32_t>(pc1500->debugPeek(kPc1500RamStPage)) << 8;
            ramEnd = static_cast<uint32_t>(pc1500->debugPeek(kPc1500RamEndPage)) << 8;
        }
    }
    const size_t len = request.payload.size();
    const machinecode::Advice advice =
        machinecode::advice(target, slot, request.addr, len, code.autorunAddr, ramStart, ramEnd, basicAreas);

    auto hex = [](uint32_t v) { return QStringLiteral("&") + QString::number(v, 16).toUpper(); };
    QString where = tr("Loaded %1 bytes at %2–%3").arg(len).arg(hex(request.addr), hex(request.addr + len - 1));
    if (isPC1600) where += tr(" (%1)").arg(QString::fromLatin1(machinecode::slotName(slot)));
    QString html = QStringLiteral("<p>%1.</p>").arg(where.toHtmlEscaped());
    html += QStringLiteral("<p>%1<br>").arg(tr("Keep BASIC from overwriting it:").toHtmlEscaped());
    if (!advice.newCommand.empty())
        html += QStringLiteral("<b><tt>%1</tt></b><br>").arg(QString::fromStdString(advice.newCommand).toHtmlEscaped());
    html += QStringLiteral("<small>%1</small></p>").arg(QString::fromStdString(advice.newNote).toHtmlEscaped());
    html += QStringLiteral("<p>%1<br><b><tt>%2</tt></b><br><small>%3</small></p>")
                .arg(tr("Start it:").toHtmlEscaped(),
                     QString::fromStdString(advice.callCommand).toHtmlEscaped(),
                     QString::fromStdString(advice.callNote).toHtmlEscaped());

    QMessageBox box(QMessageBox::Information, tr("Machine Code Loaded"), html, QMessageBox::Ok, this);
    box.setTextFormat(Qt::RichText);
    box.setTextInteractionFlags(Qt::TextSelectableByMouse);
    box.exec();

    // Type the proposed CALL, ready to run: the user presses ENTER.
    m_controller->pasteText(advice.callCommand);
}

void MainWindow::syncPeripherals() {
    const bool ce150Attached = m_controller->ce150Attached();
    const bool ce1600pAttached = m_controller->ce1600pAttached();
    const bool ce158Attached = m_controller->ce158Attached();
    // The CE-1600P excludes both the CE-150 and (on a PC-1600) the CE-158
    // -- the CE-158 does not connect to the CE-1600P: gray out whichever
    // buttons the attached peripherals rule out.
    m_controlBar->setCe150State(ce150Attached, !ce1600pAttached);
    m_controlBar->setCe1600pState(ce1600pAttached, !ce150Attached && !ce158Attached);
    m_controlBar->setCe158State(ce158Attached, !ce1600pAttached);
    // CE-1600F attaches as a union with CE-1600P (PC1600Machine::
    // attachCE1600P()); whoever attached it (PlotterController or a
    // preset) already put its disk in. Gray the picker in/out alongside
    // it (it stays visible either way -- see syncControlBarForModel()).
    m_controlBar->setFloppyEnabled(ce1600pAttached);
    refreshFloppyCombo();
    // Re-kind on every sync, not just on a change: it also drops the paper's
    // cached plot revision, which a rebuilt machine's plotter restarts.
    if (ce150Attached || ce1600pAttached)
        m_plotterPaper->setKind(ce150Attached ? PlotterPaperWidget::Kind::CE150 : PlotterPaperWidget::Kind::CE1600P);
    setDockedPane(m_plotterPaper, ce150Attached || ce1600pAttached);
    setDockedPane(m_ce158Printer, ce158Attached);
}

void MainWindow::setDockedPane(QWidget* pane, bool show) {
    const bool docked = m_debugRowLayout->indexOf(pane) >= 0;
    if (show) {
        if (!docked) m_debugRowLayout->addWidget(pane, 1);
        pane->show();
    } else if (docked) {
        m_debugRowLayout->removeWidget(pane);
        pane->hide();
    }
}

void MainWindow::onPresetArmed() {
    // The preset attached its plotter directly on the Core machine, bypassing
    // PlotterController/attachCE150()/attachCE1600P() entirely -- the plotter
    // resync inside this picks that up from the machine's actual state.
    syncUiFromController();
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

// Every UI surface that mirrors machine state, pulled from the controller:
// called after anything rebuilds or replaces the machine (startup, model
// switch, preset load), so no call site has to know which pickers exist.
void MainWindow::syncUiFromController() {
    const Model model = m_controller->currentModel();
    m_faceplate->setModel(model);
    m_controlBar->setModel(model);
    m_controlBar->setRomRevision(m_controller->pc1500RomRevision());
    m_controlBar->setPC1600RomVersion(m_controller->pc1600RomVersion());
    m_controlBar->setCE1600PRomVersion(m_controller->ce1600pRomVersion());
    syncControlBarForModel();
    syncMachineMenuFromModel(model);
    syncMachineMenuFromRomRevision(m_controller->pc1500RomRevision());
    syncMachineMenuFromPC1600RomVersion(m_controller->pc1600RomVersion());
    syncMachineMenuFromCE1600PRomVersion(m_controller->ce1600pRomVersion());
    refreshModuleCombos();
    m_plotterController->syncFromMachineState();
}

void MainWindow::refreshModuleCombos() {
    for (int slot = 1; slot <= 2; ++slot) {
        const CardHost host = MemoryModuleManager::hostForModel(slot, m_controller->currentModel());
        const MemoryModuleManager::ModuleLists lists = m_moduleManager->moduleLists(host);
        m_controlBar->setModuleCombos(slot, lists.templates, lists.instances,
                                      m_moduleManager->selectedModuleName(slot));
        m_controlBar->setSlotSaveEnabled(slot, m_moduleManager->canNameAndSave(slot));
    }
}

void MainWindow::refreshFloppyCombo() {
    const FloppyDiskManager::DiskLists lists = m_floppyManager->diskLists();
    m_controlBar->setFloppyCombo(lists.templates, lists.instances, m_floppyManager->selectedDiskName());
    m_controlBar->setFloppySide(m_floppyManager->side());
    m_controlBar->setFloppySaveEnabled(m_floppyManager->canNameAndSave());
}

namespace {

// The physical key behind a key event, stable between its press and its
// release whatever the modifiers did in between (unlike QKeyEvent::key()).
quint32 physicalKeyId(const QKeyEvent* event) {
#ifdef Q_OS_MACOS
    // nativeScanCode() carries nothing on macOS; the virtual key code is
    // the physical key position (kVK_*), independent of the layout.
    return event->nativeVirtualKey();
#else
    if (event->nativeScanCode() != 0) return event->nativeScanCode();
    return static_cast<quint32>(event->key());
#endif
}

} // namespace

void MainWindow::keyPressEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        // Qt delivers OS auto-repeat as repeated .down events -- without
        // this, holding a shift-needing key would re-fire the whole
        // tapShiftedKey choreography on every repeat tick.
        event->accept();
        return;
    }

    // A Shift press arms the tap; any other key (letters, Cmd/Ctrl/Alt,
    // Shift+Delete, ...) means Shift is being used as a modifier.
    if (event->key() == Qt::Key_Shift) {
        armShiftTap();
        QWidget::keyPressEvent(event);
        return;
    }
    disarmShiftTap();

    const bool isPC1600 = m_controller->currentModel() == Model::PC1600;
    auto resolved = PC1500KeyboardMap::resolve(static_cast<Qt::Key>(event->key()),
                                                event->modifiers(), event->text(), isPC1600,
                                                event->nativeVirtualKey());
    if (!resolved) {
        QWidget::keyPressEvent(event);
        return;
    }

    // A real machine keystroke aborts a paste still typing -- an easy way
    // out of a long one, and it keeps the two from fighting over the key
    // matrix. (Checked after resolve() so bare modifiers -- the Cmd of a
    // second Cmd-V, Cmd-Tab -- don't count.)
    if (m_controller->pasteActive()) m_controller->cancelPaste();

    // A real held press/release, tracked for the matching .up event --
    // used for any key that bypasses the live-typing queue.
    auto trackAndPress = [this, event](const std::string& baseKey) {
        m_controller->pressKey(baseKey);
        m_physicalKeysDown.insert(physicalKeyId(event), baseKey);
    };

    if (isPC1600) {
        // PC-1600 keystroke buffering is out of scope for now -- this
        // branch is unchanged.
        if (resolved->needsShift) {
            // Self-contained fire-and-forget sequence -- nothing to track
            // for the matching .up event, so release skips it too.
            m_controller->tapShiftedKey(resolved->baseKey);
        } else {
            trackAndPress(resolved->baseKey);
        }
        event->accept();
        return;
    }

    // PC-1500: cursor keys bypass the queue so the ROM's own confirmed
    // real-hardware auto-repeat can engage on a genuine physical hold;
    // everything else (including shifted keys) is queued so fast typing
    // can't outrun the key-scan loop and lose keystrokes -- see
    // MachineController::enqueueKey()/enqueueShiftedKey().
    if (resolved->needsShift) {
        // Self-contained: the queue owns shift's whole tap-then-base-key
        // sequence, nothing to track for the matching .up event.
        m_controller->enqueueShiftedKey(resolved->baseKey);
    } else if (resolved->isPc1500RepeatKey()) {
        trackAndPress(resolved->baseKey);
    } else {
        // Self-contained: the queue owns the whole press/hold/release/idle
        // cycle, nothing to track for the matching .up event.
        m_controller->enqueueKey(resolved->baseKey);
    }
    event->accept();
}

QImage MainWindow::toQImage(const GrayImage& screen) {
    if (screen.width <= 0 || screen.height <= 0) return {};
    QImage image(screen.width, screen.height, QImage::Format_Grayscale8);
    for (int y = 0; y < screen.height; ++y) {
        std::memcpy(image.scanLine(y), screen.pixels.data() + static_cast<std::size_t>(y) * screen.width,
                    static_cast<std::size_t>(screen.width));
    }
    const int dotsPerMeter = static_cast<int>(screen.pixelsPerMeter());
    image.setDotsPerMeterX(dotsPerMeter);
    image.setDotsPerMeterY(dotsPerMeter);
    return image;
}

void MainWindow::copyScreenToClipboard() {
    const GrayImage screen = m_controller->currentScreenImage();
    const QImage image = toQImage(screen);
    if (image.isNull()) return;

#ifdef Q_OS_MACOS
    // Qt's QClipboard::setImage() drops the physical size on macOS (see
    // MacClipboardImage.h) -- go through Cocoa so a paste lands at the
    // real display's size.
    const double widthPt = screen.width * 72.0 / screen.dpi;
    const double heightPt = screen.height * 72.0 / screen.dpi;
    if (macSetClipboardImage(image, widthPt, heightPt)) return;
#endif
    QGuiApplication::clipboard()->setImage(image);
}

void MainWindow::pasteClipboardText() {
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasText()) return; // images etc. are ignored
    m_controller->pasteText(mime->text().toStdString());
}

void MainWindow::keyReleaseEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Shift) {
        const bool tapped = m_shiftTapArmed && m_shiftTapClock.elapsed() <= kShiftTapMaxMs;
        disarmShiftTap();
        if (tapped) {
            if (m_controller->pasteActive()) m_controller->cancelPaste();
            m_controller->tapKey("shift");
            event->accept();
            return;
        }
    }

    auto it = m_physicalKeysDown.find(physicalKeyId(event));
    if (it == m_physicalKeysDown.end()) {
        QWidget::keyReleaseEvent(event);
        return;
    }
    m_controller->releaseKey(it.value());
    m_physicalKeysDown.erase(it);
    event->accept();
}

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::ActivationChange && !isActiveWindow()) releaseHeldKeys();
    QMainWindow::changeEvent(event);
}

void MainWindow::releaseHeldKeys() {
    for (const std::string& key : std::as_const(m_physicalKeysDown)) m_controller->releaseKey(key);
    m_physicalKeysDown.clear();
    disarmShiftTap(); // the Shift release may land elsewhere
}

// The app-wide filter is only installed while a tap is armed (at most
// kShiftTapMaxMs), not for the window's life: it sees every event of
// every object, just to catch a Shift-click.
void MainWindow::armShiftTap() {
    if (!m_shiftTapArmed) qApp->installEventFilter(this);
    m_shiftTapArmed = true;
    m_shiftTapClock.start();
}

void MainWindow::disarmShiftTap() {
    if (!m_shiftTapArmed) return;
    m_shiftTapArmed = false;
    qApp->removeEventFilter(this);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) disarmShiftTap(); // a Shift-click
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::refreshViewsAfterAdvance() {
    const DisplayFrame frame = m_controller->currentDisplay();
    m_faceplate->lcdWidget()->setFrame(frame);
    m_faceplate->lcdWidget()->update();

    m_moduleManager->markDirtyAndSchedulePersist();
    m_floppyManager->markDirtyAndSchedulePersist();
    m_controlBar->setFloppyMotorOn(m_floppyManager->motorOn());
    m_debugPanel->onFrameTick();
    if (m_debugRowLayout->indexOf(m_plotterPaper) >= 0) m_plotterPaper->onFrameTick();
    if (m_debugRowLayout->indexOf(m_ce158Printer) >= 0) m_ce158Printer->onFrameTick();
}

bool MainWindow::loadPresetForShots(const QString& path, QString* error) {
    const bool ok = runSynchronousLoad(
        tr("Load Preset"), [this, path](QString* loadError) { return m_presetController->loadPreset(path, loadError); },
        [this] { onPresetArmed(); }, error);
    // Frozen emulation means no frame tick will show the end state -- the
    // LCD would keep the load's last throttled repaint, the paper its
    // pre-plot points.
    refreshViewsAfterAdvance();
    return ok;
}

void MainWindow::buildMenuBar() {
    // File: one-shot actions -- openPresetAction/settingsAction/aboutAction
    // are connected by the constructor, next to their handler closures.
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    m_openPresetAction = fileMenu->addAction(tr("Load Preset…"));
    m_loadBasicProgramAction = fileMenu->addAction(tr("Load BASIC Program…"));
    m_loadMachineCodeAction = fileMenu->addAction(tr("Load Machine Code…"));
    fileMenu->addSeparator();
    QAction* quitAction = fileMenu->addAction(tr("Quit"));
    quitAction->setMenuRole(QAction::QuitRole);
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    // Edit: Cmd-C / Cmd-V (Ctrl on other platforms). A focused text field
    // (e.g. the debug panel's) still gets its own copy/paste first -- Qt
    // lets a widget claim standard shortcuts via ShortcutOverride.
    // Copy is Copy Screen, or -- while text is selected in the debug
    // panel's output (which never takes focus) -- Copy Log Selection.
    QMenu* editMenu = menuBar()->addMenu(tr("&Edit"));
    QAction* copyAction = editMenu->addAction(tr("Copy Screen"));
    copyAction->setShortcut(QKeySequence::Copy);
    connect(copyAction, &QAction::triggered, this, [this] {
        const QString selection = m_debugPanel->selectedOutputText();
        if (selection.isEmpty()) {
            copyScreenToClipboard();
        } else {
            QGuiApplication::clipboard()->setText(selection);
        }
    });
    connect(m_debugPanel, &DebugPanel::outputSelectionChanged, copyAction, [copyAction, this](bool hasSelection) {
        copyAction->setText(hasSelection ? tr("Copy Log Selection") : tr("Copy Screen"));
    });
    m_pasteAction = editMenu->addAction(tr("Paste Text"));
    m_pasteAction->setShortcut(QKeySequence::Paste);
    connect(m_pasteAction, &QAction::triggered, this, &MainWindow::pasteClipboardText);
    auto refreshPasteEnabled = [this] {
        const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
        m_pasteAction->setEnabled(mime && mime->hasText());
    };
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, refreshPasteEnabled);
    refreshPasteEnabled();
    // Settings: the platform's usual place -- the application menu on macOS
    // (PreferencesRole moves it there, as "Settings…" with Cmd-,), the end
    // of the Edit menu elsewhere.
    editMenu->addSeparator();
    m_settingsAction = editMenu->addAction(tr("Settings…"));
    m_settingsAction->setMenuRole(QAction::PreferencesRole);
    m_settingsAction->setShortcut(QKeySequence::Preferences);
    if (m_settingsAction->shortcut().isEmpty())
        m_settingsAction->setShortcut(QKeySequence(Qt::ControlModifier | Qt::Key_Comma));

    // Machine: duplicates ControlBar's model/ROM pickers (checkable, exclusive
    // per group) plus Reset/Reset All -- the only place Reset lives.
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

    QMenu* rom1600Menu = machineMenu->addMenu(tr("ROM Version"));
    m_rom1600MenuAction = rom1600Menu->menuAction();
    m_rom1600ActionGroup = new QActionGroup(this);
    m_rom1600ActionGroup->setExclusive(true);
    auto addRom1600Action = [&](PC1600RomVersion version, const QString& label) {
        QAction* action = rom1600Menu->addAction(label);
        action->setCheckable(true);
        m_rom1600ActionGroup->addAction(action);
        m_rom1600Actions.insert(version, action);
        connect(action, &QAction::triggered, this, [this, version] { applyPC1600RomVersionSelection(version); });
    };
    addRom1600Action(PC1600RomVersion::New, tr("New"));
    addRom1600Action(PC1600RomVersion::Old, tr("Old"));
    m_rom1600MenuAction->setVisible(false);

    // The CE-1600P's ROM is independent of the PC-1600's (any combination is
    // possible); the CE-1600F in the same box follows it.
    QMenu* ce1600pRomMenu = machineMenu->addMenu(tr("CE-1600P ROM"));
    m_ce1600pRomMenuAction = ce1600pRomMenu->menuAction();
    m_ce1600pRomActionGroup = new QActionGroup(this);
    m_ce1600pRomActionGroup->setExclusive(true);
    auto addCE1600PRomAction = [&](CE1600PRomVersion version, const QString& label) {
        QAction* action = ce1600pRomMenu->addAction(label);
        action->setCheckable(true);
        m_ce1600pRomActionGroup->addAction(action);
        m_ce1600pRomActions.insert(version, action);
        connect(action, &QAction::triggered, this, [this, version] { applyCE1600PRomVersionSelection(version); });
    };
    addCE1600PRomAction(CE1600PRomVersion::New, tr("New"));
    addCE1600PRomAction(CE1600PRomVersion::Old, tr("Old"));
    m_ce1600pRomMenuAction->setVisible(false);

    machineMenu->addSeparator();
    QAction* resetAction = machineMenu->addAction(tr("Reset"));
    resetAction->setShortcut(QKeySequence(Qt::ControlModifier | Qt::Key_R));
    connect(resetAction, &QAction::triggered, this, [this] { resetMachine(false); });
    QAction* resetAllAction = machineMenu->addAction(tr("Reset All"));
    resetAllAction->setShortcut(QKeySequence(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_R));
    connect(resetAllAction, &QAction::triggered, this, [this] { resetMachine(true); });

    // Help
    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    m_aboutAction = helpMenu->addAction(tr("About Calc-U-1600…"));
    m_aboutAction->setMenuRole(QAction::AboutRole);
}

void MainWindow::applyModelSelection(Model model) {
    // The Machine menu's QActionGroup re-emits triggered() for the item
    // that is already checked; re-picking it must not cold-rebuild.
    if (model == m_controller->currentModel()) return;
    m_moduleManager->flushPendingPersist();
    m_floppyManager->flushPendingPersist();
    m_moduleManager->onModelChanged();
    // A model switch is a fresh default machine: A04 / new ROM, no plotter, no
    // modules (onModelChanged above), no floppy. A startup preset customises it.
    m_controller->resetRomSelectionsToDefault();
    m_controller->switchModel(model);
    m_pacer->restart(); // after the rebuild's flat-out boot
    m_floppyManager->selectDisk(QString());
    syncUiFromController();
    applyDefaultPreset(model);
}

void MainWindow::applyDefaultPreset(Model model) {
    const QString path = AppSettings::defaultPresetPath(modelSettingsKey(model));
    if (path.isEmpty()) return;
    runSynchronousLoad(
        tr("Default Preset"),
        [this, path, model](QString* error) { return m_presetController->loadDefaultPreset(path, model, error); },
        [this] { onPresetArmed(); });
}

void MainWindow::applyRomRevisionSelection(PC1500RomRevision revision) {
    if (revision == m_controller->pc1500RomRevision()) return; // see applyModelSelection()
    m_moduleManager->flushPendingPersist();
    m_floppyManager->flushPendingPersist();
    m_controller->setPC1500RomRevision(revision); // rebuilds the machine
    m_pacer->restart(); // after the rebuild's flat-out boot
    m_plotterController->syncFromMachineState(); // the plotter survives the rebuild
    m_controlBar->setRomRevision(revision);
    syncMachineMenuFromRomRevision(revision);
    refreshModuleCombos();
}

void MainWindow::applyPC1600RomVersionSelection(PC1600RomVersion version) {
    if (version == m_controller->pc1600RomVersion()) return; // see applyModelSelection()
    m_moduleManager->flushPendingPersist();
    m_floppyManager->flushPendingPersist();
    m_controller->setPC1600RomVersion(version); // rebuilds the machine when a PC-1600 is active
    m_pacer->restart(); // after the rebuild's flat-out boot
    m_plotterController->syncFromMachineState(); // the plotter survives the rebuild
    // The controller may have fallen back to New if the old ROM failed to load.
    m_controlBar->setPC1600RomVersion(m_controller->pc1600RomVersion());
    syncMachineMenuFromPC1600RomVersion(m_controller->pc1600RomVersion());
    syncControlBarForModel();
    refreshModuleCombos();
}

void MainWindow::applyCE1600PRomVersionSelection(CE1600PRomVersion version) {
    if (version == m_controller->ce1600pRomVersion()) return; // see applyModelSelection()
    m_moduleManager->flushPendingPersist();
    m_floppyManager->flushPendingPersist();
    // Rebuilds the machine only when a CE-1600P is attached; otherwise just
    // records the choice for the next attach.
    if (m_controller->setCE1600PRomVersion(version)) {
        m_pacer->restart(); // after the rebuild's flat-out boot
        m_plotterController->syncFromMachineState(); // the plotter survives the rebuild
        syncControlBarForModel();
        refreshModuleCombos();
    }
    // The controller may have fallen back to New if the old ROM failed to load.
    m_controlBar->setCE1600PRomVersion(m_controller->ce1600pRomVersion());
    syncMachineMenuFromCE1600PRomVersion(m_controller->ce1600pRomVersion());
}

void MainWindow::syncMachineMenuFromCE1600PRomVersion(CE1600PRomVersion version) {
    if (QAction* action = m_ce1600pRomActions.value(version, nullptr)) action->setChecked(true);
}

void MainWindow::syncMachineMenuFromPC1600RomVersion(PC1600RomVersion version) {
    if (QAction* action = m_rom1600Actions.value(version, nullptr)) action->setChecked(true);
}

void MainWindow::syncMachineMenuFromModel(Model model) {
    if (QAction* action = m_modelActions.value(model, nullptr)) action->setChecked(true);
}

void MainWindow::syncMachineMenuFromRomRevision(PC1500RomRevision revision) {
    if (QAction* action = m_romActions.value(revision, nullptr)) action->setChecked(true);
}
