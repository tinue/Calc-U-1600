#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

#include "ShotScenario.hpp"

class MainWindow;
class QAction;
class QWidget;

// Plays a ShotScenario against the live MainWindow and writes its captures.
//
// Timer-driven, one step per QTimer::singleShot, never blocking the GUI
// thread: a step that opens a modal dialog or a menu (both run their own
// nested event loop) is performed from a zero-delay timer *after* the next
// step is already scheduled, so the script carries on inside that nested
// loop -- captures the dialog, then closes it.
//
// The emulator is frozen (MainWindow::setEmulationFrozen) for the whole
// run; only `run:` / `key:` / `type:` steps (and preset loads) advance it,
// so repeated runs give identical images.
class ShotRunner : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString outDirOverride;  // replaces the scenario's `out:`
        QStringList only;        // shot names to run; empty = all
        bool failFast = false;
    };

    ShotRunner(MainWindow* window, ShotScenario scenario, Options options, QObject* parent = nullptr);

    // Validates `only` against the scenario, applies window size and
    // appearance, then starts stepping. finished() always follows.
    void start();

signals:
    void finished(int exitCode);

private:
    MainWindow* m_window;
    ShotScenario m_scenario;
    Options m_options;
    QString m_outDir;

    QList<int> m_shotOrder; // indices into m_scenario.shots
    int m_shotPos = 0;
    int m_stepIdx = 0;
    int m_failures = 0;
    int m_written = 0;
    bool m_done = false;
    // Native (macOS menu-bar) menus opened by `menu:` -- how many levels
    // deep, so `close` knows how many Escapes to send.
    int m_nativeMenuDepth = 0;

    const Shot& currentShot() const { return m_scenario.shots[m_shotOrder[m_shotPos]]; }
    void scheduleNext(int delayMs);
    void runNextStep();
    // Performs `step`. `delayMs` is how long to let the UI settle before
    // the next one; `deferred` (UI verbs) is run from a zero-delay timer
    // after the next step has been scheduled -- see the class comment.
    bool runStep(const ShotStep& step, int* delayMs, std::function<void()>* deferred, QString* error);
    void failStep(const QString& message, int line);
    void beginShot();
    void finishAll();

    QWidget* findWidget(const QString& objectName, QString* error) const;
    QAction* findMenuAction(const QString& path, QString* error) const;
    bool openMenu(const QString& path, std::function<void()>* deferred, QString* error);
    bool closeTopmost(QString* error);
    void closeAllTransient();
    bool capture(const ShotCaptureSpec& spec, QString* error);
    void log(const QString& message) const;
};
