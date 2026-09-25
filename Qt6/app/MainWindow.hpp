#pragma once
#include <QMainWindow>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
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
class Ce158PrinterWidget;
class MemoryModuleManager;
class FloppyDiskManager;
class PresetController;
class AudioOutput;
class EmulationPacer;

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

    // ── Scripted screenshots (screenshots/ShotRunner) ──────────────────
    // The emulator normally runs off the pacer's ~60 Hz frame timer. A shot
    // run freezes it, so every capture sees exactly the state its steps left
    // behind (no cursor blink, no clock drift between runs), and advances
    // it explicitly instead (see EmulationPacer).
    EmulationPacer* pacer() const { return m_pacer; }
    // Load Preset… without the file dialog, and with a failure reported
    // through `error` instead of a blocking warning box.
    bool loadPresetForShots(const QString& path, QString* error);
    // Reset / Reset All, as the Machine menu does.
    void resetForShots(bool allReset) {
        resetMachine(allReset);
        refreshViewsAfterAdvance();
    }
    // True while runSynchronousLoad() is mid-load: it pumps the event loop
    // (timers included), and nothing may drive the machine until it's done.
    bool isLoading() const { return m_loading; }
    MachineController* controller() const { return m_controller.get(); }
    // `screen` (MachineController::currentScreenImage()) as a QImage at its
    // physical size (dots per metre set), as Copy Screen puts it on the
    // clipboard; null for an empty screen.
    static QImage toQImage(const GrayImage& screen);
    FaceplateWidget* faceplate() const { return m_faceplate; }
    PlotterPaperWidget* plotterPaper() const { return m_plotterPaper; }

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

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
    Ce158PrinterWidget* m_ce158Printer = nullptr; // added to m_debugRowLayout only while a CE-158 is attached
    QWidget* m_debugRow = nullptr;
    QHBoxLayout* m_debugRowLayout = nullptr;
    AudioOutput* m_audio = nullptr;
    EmulationPacer* m_pacer = nullptr; // owns the frame timer; calls refreshViewsAfterAdvance()

    void buildMenuBar();

    // Edit menu: Copy Screen puts a PNG of the LCD dot matrix (physical
    // size, no annunciators) on the clipboard; Paste Text types the
    // clipboard's text into the machine (MachineController::pasteText()).
    void copyScreenToClipboard();
    void pasteClipboardText();
    QAction* m_pasteAction = nullptr;

    // Shared by both ControlBar's combo-box signal and the Machine menu's
    // QActions, so either source of a model/ROM-revision change drives the
    // exact same rebuild + resync path (previously these were ControlBar-
    // only lambdas in the constructor).
    void applyModelSelection(Model model);
    void applyRomRevisionSelection(PC1500RomRevision revision);
    void applyPC1600RomVersionSelection(PC1600RomVersion version);
    void applyCE1600PRomVersionSelection(CE1600PRomVersion version);
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
    void syncMachineMenuFromPC1600RomVersion(PC1600RomVersion version);
    void syncMachineMenuFromCE1600PRomVersion(CE1600PRomVersion version);

    QHash<Model, QAction*> m_modelActions;
    QHash<PC1500RomRevision, QAction*> m_romActions;
    QHash<PC1600RomVersion, QAction*> m_rom1600Actions;
    QActionGroup* m_rom1600ActionGroup = nullptr;
    QAction* m_rom1600MenuAction = nullptr; // Machine > ROM Version submenu (PC-1600 only)
    QHash<CE1600PRomVersion, QAction*> m_ce1600pRomActions;
    QActionGroup* m_ce1600pRomActionGroup = nullptr;
    QAction* m_ce1600pRomMenuAction = nullptr; // Machine > CE-1600P ROM submenu (PC-1600 only)
    QActionGroup* m_modelActionGroup = nullptr;
    QActionGroup* m_romActionGroup = nullptr;
    QAction* m_romMenuAction = nullptr; // Machine > ROM Revision submenu's own action, for show/hide

    // File/Help actions whose handlers are wired up in the constructor
    // (alongside the equivalent ControlBar signal), not inside
    // buildMenuBar() itself, so both share one closure over local
    // constructor state (m_presetController, m_frameTimer, ...).
    QAction* m_openPresetAction = nullptr;
    QAction* m_loadBasicProgramAction = nullptr;
    QAction* m_loadMachineCodeAction = nullptr;
    QAction* m_settingsAction = nullptr;
    QAction* m_aboutAction = nullptr;

    void syncUiFromController();
    void refreshModuleCombos();
    void refreshFloppyCombo();

    // Shows/hides the PC-1600-only control-bar widgets (slot 2, CE-1600P
    // toggle) to match the current model. Called after every model switch
    // (manual, module-driven rebuild, or preset load).
    void syncControlBarForModel();

    // PlotterController::attachStateChanged handler: reads the CE-150/
    // CE-1600P/CE-158 attach states from MachineController and matches the
    // control-bar buttons, the floppy picker and the docked panes to them.
    void syncPeripherals();
    // Docks `pane` into (or takes it out of) the debug row.
    void setDockedPane(QWidget* pane, bool show);

    // PresetController::armed handler: the preset has attached its
    // model/cards/plotter but the machine is still powered off. Resyncs
    // every GUI element that reflects that state (faceplate/control-bar
    // model, plotter-paper visibility, slot selectors) and forces a repaint
    // before the (possibly long) boot + preset script actually runs, so the
    // user briefly sees the armed-but-off machine.
    void onPresetArmed();

    // Shared choreography for Load Preset/Load BASIC Program: stops the
    // frame timer and shows a wait cursor around the (synchronous) `loadFn`
    // call so nothing else drives the machine mid-script, and runs
    // `afterLoad` (if given) before the timer restarts. Returns `loadFn`'s
    // result. On failure its error goes to `error` when the caller passes
    // one (scripted callers), else into a warning dialog titled `title`.
    bool runSynchronousLoad(const QString& title, const std::function<bool(QString*)>& loadFn,
                            const std::function<void()>& afterLoad = {}, QString* error = nullptr);

    // File > Load Machine Code…: pick a .bin (Settings' Assembly folder),
    // recognise its header, ask for a start address / PC-1600 slot only when
    // needed (MachineCodeLoadDialog), write it, then show the NEW that
    // protects it and the CALL that starts it. Never runs the code.
    void loadMachineCode();
    // Reset / Reset All (control bar, Machine menu): see resetMachine() in
    // MainWindow.cpp.
    void resetMachine(bool allReset);

    // Physical host key (physicalKeyId()) -> the logical calculator key
    // name that was pressed for it, so releaseEvent always releases exactly
    // what pressEvent pressed even with multiple physical keys held.
    // Keyed by the physical key, not Qt::Key: key() follows the modifier
    // state, so on e.g. a Swiss layout Shift+3 presses as Key_Asterisk but,
    // with Shift let go first, releases as Key_3 -- the release would miss
    // and leave "*" held in the machine's matrix. Shift-tap (needsShift)
    // presses are never entered here since they're self-contained (see
    // MachineController::tapShiftedKey).
    QHash<quint32, std::string> m_physicalKeysDown;
    // Releases everything in m_physicalKeysDown. Used whenever the window
    // stops receiving key events (deactivation, focus moving to another
    // widget) -- the matching release would never reach us.
    void releaseHeldKeys();

    // Host Shift tapped on its own (pressed and released within
    // kShiftTapMaxMs, nothing else in between) taps the calculator's SHIFT,
    // latching it for the next key. Armed on the Shift press; any other key
    // press, a mouse press, or losing focus disarms it.
    bool m_shiftTapArmed = false;
    QElapsedTimer m_shiftTapClock;
    void armShiftTap();
    void disarmShiftTap();

    // The per-frame view refresh (LCD, debug log, paper, floppy lamp,
    // persistence) -- run by the pacer after each advance.
    void refreshViewsAfterAdvance();
    bool m_loading = false; // see isLoading()
};
