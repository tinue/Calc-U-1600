#pragma once
#include <QWidget>
#include <cstdint>
#include <vector>

#include "Connector/AlpsPlotterMechanism.hpp"
#include "PaperGeometry.hpp"

class QScrollArea;
class QPushButton;
class QLabel;
class MachineController;

// Docked plotter-paper view. CE-150 and CE-1600P geometry differs only in
// their constants, so one widget serves both: they're mutually exclusive on
// a running machine (Core-enforced), so only one is ever shown at a time --
// MainWindow calls setKind() when the active plotter changes.
//
// Layout: a title header matching DebugPanel's "DEBUG LOG" header (same
// size/coloring, showing "CE-150"/"PC-1600P" instead) and a Copy/Cut button
// row underneath the paper matching DebugPanel's TRACE/LOG row in size and
// using the exact same ChromeColors -- so this panel reads as a sibling of
// DebugPanel, not a separate visual language. No "Copied" toast: the
// buttons themselves show clear pressed feedback instead (see
// applyChrome()'s pill button stylesheet).
//
// Lives in MainWindow's debug row (see MainWindow::m_debugRowLayout) at
// stretch 1 alongside DebugPanel's stretch 2, added/removed from that layout
// as plotters attach/detach -- not always visible, unlike DebugPanel.
class PlotterPaperWidget : public QWidget {
    Q_OBJECT
public:
    enum class Kind { CE150, CE1600P };

    explicit PlotterPaperWidget(MachineController* controller, QWidget* parent = nullptr);

    void setKind(Kind kind);

    // Revision-gated point refresh, called once per ~60Hz frame tick from
    // MainWindow::onFrameTick() while a plotter is attached -- an O(1) "did
    // it change" poll before paying for the full point-vector copy.
    void onFrameTick();

protected:
    // Re-applies theme-aware chrome colors on a live light/dark switch --
    // same approach as DebugPanel::changeEvent()/applyChrome() (see
    // ChromeColors.hpp), so both panels always agree.
    void changeEvent(QEvent* event) override;

private:
    class PlotArea; // custom-painted QWidget, defined in the .cpp

    MachineController* m_controller; // not owned
    Kind m_kind = Kind::CE1600P;
    PaperGeometry m_geometry = PaperGeometry::ce1600p();

    QLabel* m_header;
    PlotArea* m_plotArea;
    QScrollArea* m_scrollArea;
    QWidget* m_buttonBar;
    QPushButton* m_copyButton;
    QPushButton* m_cutButton;

    std::vector<AlpsPlotterMechanism::FlatPoint> m_points;
    // Deliberately starts at a value revision() can never legitimately be
    // right after an attach (0 is AlpsPlotterMechanism's own initial value,
    // so this forces the very first onFrameTick() to always refresh).
    std::uint64_t m_lastRevision = UINT64_MAX;

    // Both bounds in one O(n) pass over m_points -- callers previously
    // rescanned m_points once per bound needed.
    std::pair<std::int32_t, std::int32_t> penYRange() const;
    bool isNearBottom() const;
    void scrollToBottom();
    void updateButtonsEnabled();
    void applyChrome();
    void copyToClipboard();
    void cutPaper();
};
