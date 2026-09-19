#pragma once
#include <QMainWindow>
#include <QHash>
#include <QString>
#include <functional>
#include <memory>
#include <string>

#include "MachineController.hpp" // Model, PC1500RomRevision (menu<->ControlBar sync)

class QTimer;
class QKeyEvent;
class QCloseEvent;
class QHBoxLayout;
class QAction;
class QActionGroup;
class FaceplateWidget;
class ControlBar;
class DebugPanel;
class PlotterController;
class PlotterPaperWidget;
class MemoryModuleManager;
class FloppyDiskManager;
class PresetController;

// Top-level window: FaceplateWidget (stretch) over ControlBar (fixed) over
// the debug row (fixed), all inside a central QWidget (QMainWindow requires
// exactly one); the module pickers live in ControlBar itself. The debug row
// is a QHBoxLayout (m_debugRow / m_debugRowLayout) holding DebugPanel at
// stretch 2 always, plus PlotterPaperWidget at stretch 1 whenever a plotter
// is attached (a fixed 2:1 split, full width when the paper widget is
// absent). Owns the MachineController and the single ~60Hz frame timer that
// both advances emulation and repaints the LCD. Also owns physical-keyboard
// capture -- see PC1500KeyboardMap.hpp.
//
// The menu bar (buildMenuBar()) duplicates ControlBar's model/ROM-revision
// pickers and Reset/Reset All as QActions, and adds File/Help entries
// (Load Preset/Load BASIC Program/Settings/Quit, About) -- see
// applyModelSelection()/applyRomRevisionSelection() and the
// syncMachineMenuFrom*() pair for how the two views of the same
// MachineController state stay in sync without fighting each other.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    std::unique_ptr<MachineController> m_controller;
    std::unique_ptr<MemoryModuleManager> m_moduleManager;
    std::unique_ptr<FloppyDiskManager> m_floppyManager;
    std::unique_ptr<PresetController> m_presetController;
    std::unique_ptr<PlotterController> m_plotterController;
    FaceplateWidget* m_faceplate = nullptr;
    ControlBar* m_controlBar = nullptr;
    DebugPanel* m_debugPanel = nullptr;
    PlotterPaperWidget* m_plotterPaper = nullptr; // added to m_debugRowLayout only while a plotter is attached
    bool m_plotterPaperInLayout = false;
    QWidget* m_debugRow = nullptr;
    QHBoxLayout* m_debugRowLayout = nullptr;
    QTimer* m_frameTimer = nullptr;
    bool m_turboActive = false; // press-and-hold on the LCD: run unthrottled

    void buildMenuBar();

    // Shared by both ControlBar's combo-box signal and the Machine menu's
    // QActions, so either source of a model/ROM-revision change drives the
    // exact same rebuild + resync path (previously these were ControlBar-
    // only lambdas in the constructor).
    void applyModelSelection(Model model);
    void applyRomRevisionSelection(PC1500RomRevision revision);
    // Loads `model`'s default preset (AppSettings::defaultPresetPath()), if
    // one is set -- run whenever a model gets selected, including at startup.
    void applyDefaultPreset(Model model);

    // Re-checks the Machine menu's model/ROM QActions to match `model`/
    // `revision` without themselves triggering another applyModelSelection/
    // applyRomRevisionSelection -- QAction::setChecked() doesn't emit
    // triggered() on its own (only user activation does), so this is safe
    // to call from applyModelSelection() itself and from every place that
    // resyncs ControlBar (constructor, onPresetArmed()).
    void syncMachineMenuFromModel(Model model);
    void syncMachineMenuFromRomRevision(PC1500RomRevision revision);

    QHash<Model, QAction*> m_modelActions;
    QHash<PC1500RomRevision, QAction*> m_romActions;
    QActionGroup* m_modelActionGroup = nullptr;
    QActionGroup* m_romActionGroup = nullptr;
    QAction* m_romMenuAction = nullptr; // Machine > ROM Revision submenu's own action, for show/hide

    // File/Help actions whose handlers are wired up in the constructor
    // (alongside the equivalent ControlBar signal), not inside
    // buildMenuBar() itself, so both share one closure over local
    // constructor state (m_presetController, m_frameTimer, ...).
    QAction* m_openPresetAction = nullptr;
    QAction* m_loadBasicProgramAction = nullptr;
    QAction* m_settingsAction = nullptr;
    QAction* m_aboutAction = nullptr;

    void refreshModuleCombos();
    void refreshFloppyCombo();

    // Shows/hides the PC-1600-only control-bar widgets (slot 2, CE-1600P
    // toggle) to match the current model. Called after every model switch
    // (manual, module-driven rebuild, or preset load).
    void syncControlBarForModel();

    // CE-150/CE-1600P attach-state handler, shared by both
    // PlotterController::ce150AttachedChanged/ce1600pAttachedChanged
    // signals: they only differ in which plotter is "self" vs "other".
    void onPlotterAttachedChanged(bool isCE150, bool attached);

    // PresetController::armed handler: the preset has attached its
    // model/cards/plotter but the machine is still powered off. Resyncs
    // every GUI element that reflects that state (faceplate/control-bar
    // model, plotter-paper visibility, slot selectors) and forces a repaint
    // before the (possibly long) boot + preset script actually runs, so the
    // user briefly sees the armed-but-off machine.
    void onPresetArmed();

    // Shared choreography for Load Preset/Load BASIC Program: stops the
    // frame timer and shows a wait cursor around the (synchronous) `loadFn`
    // call so nothing else drives the machine mid-script, runs `afterLoad`
    // (if given) before the timer restarts, then reports `loadFn`'s error
    // via a warning dialog titled `errorTitle` on failure.
    void runSynchronousLoad(const QString& errorTitle, const std::function<bool(QString*)>& loadFn,
                             const std::function<void()>& afterLoad = {});

    // Qt::Key -> the logical calculator key name that was pressed for it,
    // so releaseEvent always releases exactly what pressEvent pressed even
    // with multiple physical keys held -- more robust than re-resolving on
    // release, whose modifier state may have already changed (e.g. Shift
    // released first). Shift-tap (needsShift) presses are never entered
    // here since they're self-contained (see MachineController::tapShiftedKey).
    QHash<int, std::string> m_physicalKeysDown;

    void onFrameTick();
};
