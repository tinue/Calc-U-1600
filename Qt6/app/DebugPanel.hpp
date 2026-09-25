#pragma once
#include <QColor>
#include <QIcon>
#include <QWidget>
#include <cstdint>
#include <string>
#include <vector>

class QPlainTextEdit;
class QPushButton;
class QToolButton;
class QLabel;
class MachineController;
class MemoryModuleManager;

// Docked debug/trace console: a header, a scrolling monospaced output log,
// and a two-row button bar (Pointers / Dump Mem / Dump Card YAML / Clear,
// then TRACE / LOG toggle pills). Always visible under the control bar
// (see MainWindow).
//
// A plain QWidget (not a QDialog, unlike SettingsDialog) so it docks inline
// rather than floating -- see MainWindow's m_debugRow, which also reserves
// the layout slot a future plotter-paper widget will share it with at a
// 2:1 width split.
class DebugPanel : public QWidget {
    Q_OBJECT
public:
    explicit DebugPanel(MachineController* controller, QWidget* parent = nullptr);
    ~DebugPanel();

    // Late-bound, mirroring MachineController::setModuleManager's own
    // construction-order workaround (MemoryModuleManager doesn't exist yet
    // when MainWindow constructs this panel).
    void setModuleManager(MemoryModuleManager* mgr) { m_moduleManager = mgr; }

    // Called once per ~60Hz frame tick from MainWindow::onFrameTick() --
    // checks the trace file against its size cap while TRACE is enabled.
    void onFrameTick();

    // Text selected (with the mouse) in the output area, "" if none.
    // `outputSelectionChanged` fires when that changes -- Edit > Copy then
    // copies it instead of the screen.
    QString selectedOutputText() const;

signals:
    void outputSelectionChanged(bool hasSelection);

protected:
    // Re-applies theme-aware chrome colors when the system light/dark
    // appearance changes (QEvent::PaletteChange/ApplicationPaletteChange) --
    // see applyChrome()'s own doc comment for why the container backgrounds,
    // not just the buttons, need to track this.
    void changeEvent(QEvent* event) override;

private:
    MachineController* m_controller;               // not owned
    MemoryModuleManager* m_moduleManager = nullptr; // not owned, set later

    QLabel* m_header = nullptr;
    QWidget* m_buttonBar = nullptr;
    QPlainTextEdit* m_output = nullptr;
    QPushButton* m_pointersButton = nullptr;
    QPushButton* m_dumpMemButton = nullptr;
    QPushButton* m_dumpCardButton = nullptr;
    QPushButton* m_clearButton = nullptr;
    QToolButton* m_traceButton = nullptr;
    QToolButton* m_logButton = nullptr;

    // ── Ring buffer / paging ─────────────────────────────────────────────
    static constexpr int kRingBufCap = 2000;
    static constexpr int kDisplayLinesPerPage = 40;
    static constexpr int kDisplayPageCap = 10;
    std::vector<std::string> m_ringBuf = std::vector<std::string>(kRingBufCap);
    int m_ringHead = 0;
    int m_debugLastDisplayHead = 0;
    void ringWrite(const std::string& line);
    void ringWriteAll(const std::vector<std::string>& lines);
    void refreshDebugDisplay();

    // ── TRACE ─────────────────────────────────────────────────────────
    // The capture itself runs inside Core (MachineController::beginTrace());
    // this panel only starts/stops it and enforces the
    // AppSettings::traceMaxFileSizeMB() cap from Core's running byte count.
    // Whether a capture runs is MachineController::traceActive();
    // m_traceShownActive is only what the button currently shows, so the
    // next frame notices a capture a machine rebuild ended.
    bool m_traceShownActive = false;
    std::uint64_t m_traceMaxBytes = 0;
    void setTraceEnabled(bool enabled);
    void updateTraceButtonAppearance();

    // ── LOG (cosmetic placeholder -- Core has no log statements yet) ─────
    enum class DebugLevel { Off, Info, Debug } m_debugLevel = DebugLevel::Off;
    void toggleDebugLevel();
    void updateLogButtonAppearance();

    // ── Dump commands ─────────────────────────────────────────────────
    void debugDumpPointers();
    void debugDumpPointersPC1500();
    void debugDumpPointersPC1600();
    void debugDumpMemory();
    void debugDumpMemoryPC1500();
    void debugDumpMemoryPC1600();
    void debugDumpModuleCardAsYaml();
    void clearDebug();

    struct MemRegion { int start; int end; std::string label; bool hasLabel; bool suppressDump = false; };
    std::vector<std::string> debugDumpMemRegion(const MemRegion& region);
    static std::string debugSizeLabel(int bytes);
    bool batteryCardImage(int slot, int* bankCount, std::vector<std::uint8_t>* image);

    QIcon dotIcon(const QColor& color) const;

    // ── Theming ──────────────────────────────────────────────────────
    //
    // The window chrome and native controls track the system light/dark
    // appearance, so the header/output/button-bar backgrounds need to as
    // well: a hardcoded-light backdrop puts native dark-mode buttons on a
    // surface they're not designed for -- macOS renders a dark-mode
    // QPushButton assuming a dark surface behind it, so its text reads as
    // washed-out/disabled on a light bar. This panel tracks the system
    // appearance with two hand-picked palettes (light and dark, matched for
    // relative contrast/hierarchy), applied to the header/output/button-bar
    // containers so native buttons always sit on a backdrop their own
    // platform styling expects. Re-applied on QEvent::PaletteChange (see
    // changeEvent()) so a live light/dark switch updates without a relaunch.
    bool isDarkMode() const;
    void applyChrome();
    QColor m_pillOnColor;
    QColor m_pillOffColor;
    QColor m_pillTextColor;
};
