#pragma once
#include <QWidget>
#include <QHash>
#include <memory>
#include <string>

class QTimer;
class QKeyEvent;
class QCloseEvent;
class QHBoxLayout;
class FaceplateWidget;
class ControlBar;
class DebugPanel;
class PlotterController;
class PlotterPaperWidget;
class MachineController;
class MemoryModuleManager;
class PresetController;

// Top-level window: FaceplateWidget (stretch) over ControlBar (fixed) over
// the debug row (fixed); the module pickers live in ControlBar itself. The
// debug row is a QHBoxLayout (m_debugRow / m_debugRowLayout) holding
// DebugPanel at stretch 2 always, plus PlotterPaperWidget at stretch 1
// whenever a plotter is attached (a fixed 2:1 split, full width when the
// paper widget is absent). Owns the MachineController and the single
// ~60Hz frame timer that both advances emulation and repaints the LCD.
// Also owns physical-keyboard capture -- see PC1500KeyboardMap.hpp.
class MainWindow : public QWidget {
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

    void refreshModuleCombos();

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

    // Qt::Key -> the logical calculator key name that was pressed for it,
    // so releaseEvent always releases exactly what pressEvent pressed even
    // with multiple physical keys held -- more robust than re-resolving on
    // release, whose modifier state may have already changed (e.g. Shift
    // released first). Shift-tap (needsShift) presses are never entered
    // here since they're self-contained (see MachineController::tapShiftedKey).
    QHash<int, std::string> m_physicalKeysDown;

    void onFrameTick();
};
