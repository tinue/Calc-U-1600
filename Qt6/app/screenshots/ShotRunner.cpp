#include "ShotRunner.hpp"
#include "ShotCapture.hpp"
#include "MacShotSupport.h"

#include "../FaceplateWidget.hpp"
#include "../EmulationPacer.hpp"
#include "../MainWindow.hpp"
#include "../MachineController.hpp"
#include "../PlotterPaperWidget.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QGuiApplication>
#include <QMenu>
#include <QMenuBar>
#include <QProcess>
#include <QScreen>
#include <QStyleHints>
#include <QTimer>
#include <cstdio>

namespace {

// Wall-clock pauses: long enough for the window server to show/hide a
// native menu that osascript opened or closed.
constexpr int kNativeMenuSettleMs = 1000;
constexpr int kNativeMenuCloseMs = 600;
// Before the first step: the window must be exposed and laid out.
constexpr int kStartupDelayMs = 800;
// Emulated time a `key:` tap holds / then lets the ROM react.
constexpr double kKeyHoldSeconds = 0.1;
constexpr double kKeyReactSeconds = 0.2;
// `type:` gives up on a line still being typed after this much.
constexpr double kTypeCapSeconds = 120.0;

// Menu titles as the user sees them: no mnemonic '&', "..." == "…".
QString normalizedMenuText(QString text) {
    text.remove(QLatin1Char('&'));
    text.replace(QStringLiteral("..."), QStringLiteral("…"));
    return text.trimmed();
}

QStringList menuPath(const QString& path) {
    QStringList parts;
    for (const QString& part : path.split(QLatin1Char('>'))) parts << normalizedMenuText(part);
    return parts;
}

QString appleScriptString(const QString& s) {
    QString escaped = s;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QDialog* topmostDialog() {
    if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) return modal;
    QDialog* last = nullptr;
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (auto* d = qobject_cast<QDialog*>(w); d && d->isVisible()) last = d;
    }
    return last;
}

} // namespace

ShotRunner::ShotRunner(MainWindow* window, ShotScenario scenario, Options options, QObject* parent)
    : QObject(parent), m_window(window), m_scenario(std::move(scenario)), m_options(std::move(options)) {}

void ShotRunner::log(const QString& message) const {
    std::fprintf(stderr, "[shots] %s\n", qPrintable(message));
    std::fflush(stderr);
}

void ShotRunner::start() {
    m_outDir = m_options.outDirOverride.isEmpty() ? m_scenario.outDir
                                                  : QDir(m_options.outDirOverride).absolutePath();
    for (int i = 0; i < m_scenario.shots.size(); ++i) {
        if (m_options.only.isEmpty() || m_options.only.contains(m_scenario.shots[i].name)) m_shotOrder.append(i);
    }
    for (const QString& name : m_options.only) {
        bool known = false;
        for (const Shot& shot : m_scenario.shots) known = known || shot.name == name;
        if (!known) {
            log(QStringLiteral("%1: no shot named '%2'").arg(m_scenario.path, name));
            m_failures++;
        }
    }
    if (m_failures > 0 || m_shotOrder.isEmpty()) {
        QTimer::singleShot(0, this, [this] { finishAll(); });
        return;
    }

    // setColorScheme is Qt 6.8+; Linux CI builds against Ubuntu's Qt 6.4,
    // where `appearance:` is ignored and shots follow the system scheme.
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if (m_scenario.appearance == QLatin1String("light"))
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
    else if (m_scenario.appearance == QLatin1String("dark"))
        QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
#endif

    m_window->pacer()->setFrozen(true);
    // A fixed spot on the primary screen, so `method: system` regions and
    // popup placement are the same every run -- right under the menu bar,
    // so a menu-bar shot has no strip of desktop between the two.
    if (QScreen* screen = QGuiApplication::primaryScreen())
        m_window->move(screen->availableGeometry().topLeft() + QPoint(40, 0));
    m_window->show();
    m_window->raise();
    m_window->activateWindow();
#ifdef Q_OS_MACOS
    macActivateApp();
#endif
    log(QStringLiteral("%1: %2 shot(s) -> %3").arg(m_scenario.path).arg(m_shotOrder.size()).arg(m_outDir));
    scheduleNext(kStartupDelayMs);
}

void ShotRunner::scheduleNext(int delayMs) {
    QTimer::singleShot(delayMs, this, [this] { runNextStep(); });
}

void ShotRunner::runNextStep() {
    if (m_done) return;
    // A preset load / power cycle pumps the event loop (and so our timers)
    // while it runs -- never step into the middle of one.
    if (m_window->isLoading()) {
        scheduleNext(50);
        return;
    }
    const Shot& shot = currentShot();
    if (m_stepIdx == 0) {
        log(QStringLiteral("shot '%1'").arg(shot.name));
        applyWindowSize();
    }
    if (m_stepIdx >= shot.steps.size()) {
        m_window->faceplate()->releasePressedKey();
        closeAllTransient();
        const int delay = m_nativeMenuDepth > 0 ? kNativeMenuCloseMs : m_scenario.settleMs;
        m_nativeMenuDepth = 0;
        m_stepIdx = 0;
        if (++m_shotPos >= m_shotOrder.size()) {
            finishAll();
            return;
        }
        scheduleNext(delay);
        return;
    }

    const ShotStep& step = shot.steps[m_stepIdx++];
    int delay = 0;
    std::function<void()> deferred;
    QString error;
    if (!runStep(step, &delay, &deferred, &error)) {
        failStep(error, step.line);
        if (!m_done) scheduleNext(0);
        return;
    }
    if (deferred) {
        // Next step first (non-zero delay, so it fires after `deferred`),
        // then the action that may enter a nested event loop.
        scheduleNext(std::max(delay, 20));
        QTimer::singleShot(0, this, deferred);
    } else {
        scheduleNext(delay);
    }
}

QSize ShotRunner::wantedWindowSize() const {
    const Shot& shot = currentShot();
    return shot.windowSize.isValid() ? shot.windowSize : m_scenario.windowSize;
}

void ShotRunner::applyWindowSize() {
    const QSize wanted = wantedWindowSize();
    if (!wanted.isValid()) return;
    // Let a model switch's new minimum size (the PC-1600 control bar is
    // much wider) reach the layout first, or it still pins the old one.
    QCoreApplication::processEvents();
    m_window->resize(wanted);
    QCoreApplication::processEvents();
}

void ShotRunner::failStep(const QString& message, int line) {
    m_failures++;
    log(QStringLiteral("%1:%2: shot '%3': %4").arg(m_scenario.path).arg(line).arg(currentShot().name, message));
    m_stepIdx = currentShot().steps.size(); // skip the rest of this shot
    if (m_options.failFast) finishAll();
}

void ShotRunner::finishAll() {
    if (m_done) return;
    m_done = true;
    if (m_window->faceplate()) m_window->faceplate()->releasePressedKey();
    closeAllTransient();
    log(QStringLiteral("%1 image(s) written, %2 failure(s)").arg(m_written).arg(m_failures));
    emit finished(m_failures > 0 ? 1 : 0);
}

bool ShotRunner::menuScriptError(QString* error) {
    if (m_nativeMenuDepth == 0 || !m_menuScript || m_menuScript->state() != QProcess::NotRunning) return false;
    if (m_menuScript->exitStatus() == QProcess::NormalExit && m_menuScript->exitCode() == 0) return false;
    *error = QStringLiteral("could not open the native menu (%1). The app launching this run needs the "
                            "Automation (System Events) and Accessibility permissions (System Settings > "
                            "Privacy & Security).")
                 .arg(QString::fromLocal8Bit(m_menuScript->readAllStandardError()).trimmed());
    m_nativeMenuDepth = 0;
    return true;
}

void ShotRunner::releaseKey() {
    m_window->faceplate()->releasePressedKey();
    m_window->pacer()->runFor(kKeyReactSeconds);
}

bool ShotRunner::runStep(const ShotStep& step, int* delayMs, std::function<void()>* deferred, QString* error) {
    using K = ShotStep::Kind;
    if (menuScriptError(error)) return false;
    const int settle = m_scenario.settleMs;
    FaceplateWidget* faceplate = m_window->faceplate();
    switch (step.kind) {
    case K::Preset:
        if (!m_window->loadPresetForShots(step.text, error)) return false;
        applyWindowSize(); // the preset may have switched model
        *delayMs = settle;
        return true;
    case K::Reset:
        m_window->resetForShots(step.number == 1);
        applyWindowSize();
        *delayMs = settle;
        return true;
    case K::Key:
    case K::HoldKey:
        if (!faceplate->pressKeyByName(step.text)) {
            *error = QStringLiteral("this model has no key named '%1'").arg(step.text);
            return false;
        }
        m_window->pacer()->runFor(kKeyHoldSeconds);
        if (step.kind == K::Key) releaseKey();
        return true;
    case K::ReleaseKeys:
        releaseKey();
        return true;
    case K::Type:
        // Paste Text types one line and never presses ENTER -- tap it here.
        m_window->controller()->pasteText(step.text.toStdString());
        if (!m_window->pacer()->runUntilPasteDone(kTypeCapSeconds)) {
            *error = QStringLiteral("'type' still typing after %1 s").arg(kTypeCapSeconds);
            return false;
        }
        faceplate->pressKeyByName(QStringLiteral("enter"));
        m_window->pacer()->runFor(kKeyHoldSeconds);
        releaseKey();
        return true;
    case K::Run:
        m_window->pacer()->runFor(step.number);
        return true;
    case K::Settle:
        *delayMs = static_cast<int>(step.number);
        return true;
    case K::Click:
    case K::Open: {
        QWidget* w = findWidget(step.text, error);
        if (!w) return false;
        if (!w->isVisible() || !w->isEnabled()) {
            *error = QStringLiteral("'%1' is hidden or disabled").arg(step.text);
            return false;
        }
        if (auto* combo = qobject_cast<QComboBox*>(w); combo && step.kind == K::Open) {
            *deferred = [combo] { combo->showPopup(); };
        } else if (auto* button = qobject_cast<QAbstractButton*>(w)) {
            *deferred = [button] { button->click(); };
        } else {
            *error = QStringLiteral("'%1' is not a %2").arg(step.text, step.kind == K::Open ? "combo box or button"
                                                                                               : "button");
            return false;
        }
        *delayMs = settle;
        return true;
    }
    case K::Select: {
        QWidget* w = findWidget(step.text, error);
        if (!w) return false;
        auto* combo = qobject_cast<QComboBox*>(w);
        if (!combo) {
            *error = QStringLiteral("'%1' is not a combo box").arg(step.text);
            return false;
        }
        const int index = combo->findText(step.text2);
        if (index < 0) {
            QStringList items;
            for (int i = 0; i < combo->count(); ++i)
                if (!combo->itemText(i).isEmpty()) items << combo->itemText(i);
            *error = QStringLiteral("'%1' has no item '%2' (has: %3)")
                         .arg(step.text, step.text2, items.join(QStringLiteral(", ")));
            return false;
        }
        *deferred = [combo, index] { combo->setCurrentIndex(index); };
        *delayMs = settle;
        return true;
    }
    case K::Menu:
        if (!openMenu(step.text, deferred, error)) return false;
        *delayMs = m_nativeMenuDepth > 0 ? std::max(settle, kNativeMenuSettleMs) : settle;
        return true;
    case K::Action: {
        QAction* action = findMenuAction(step.text, error);
        if (!action) return false;
        if (action->menu()) {
            *error = QStringLiteral("'%1' is a submenu -- use 'menu:' to open it").arg(step.text);
            return false;
        }
        if (!action->isEnabled()) {
            *error = QStringLiteral("'%1' is disabled").arg(step.text);
            return false;
        }
        *deferred = [action] { action->trigger(); };
        *delayMs = settle;
        return true;
    }
    case K::Close: {
        const bool native = m_nativeMenuDepth > 0;
        if (!closeTopmost(error)) return false;
        *delayMs = native ? std::max(settle, kNativeMenuCloseMs) : settle;
        return true;
    }
    case K::ChooseFile: {
        QFileDialog* dialog = nullptr;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (auto* d = qobject_cast<QFileDialog*>(w); d && d->isVisible()) dialog = d;
        }
        if (!dialog) {
            *error = QStringLiteral("no file dialog is open");
            return false;
        }
        const QString path = step.text;
        // Deferred: accepting returns from the dialog's exec(), and the
        // code after it may open the next dialog or start a load.
        *deferred = [dialog, path] {
            dialog->setDirectory(QFileInfo(path).absolutePath());
            dialog->selectFile(path);
            // selectFile() lands asynchronously (the file model loads the
            // folder in the background); the file-name field is what
            // accept() reads, so fill it directly too.
            if (auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"))) name->setText(path);
            static_cast<QDialog*>(dialog)->accept(); // QFileDialog::accept() is protected
        };
        *delayMs = settle;
        return true;
    }
    case K::EnterText: {
        QDialog* dialog = topmostDialog();
        auto* edit = qobject_cast<QLineEdit*>(QApplication::focusWidget());
        if (!edit && dialog) edit = dialog->findChild<QLineEdit*>();
        if (!edit) {
            *error = QStringLiteral("no text field to enter text into");
            return false;
        }
        edit->setText(step.text);
        *delayMs = settle;
        return true;
    }
    case K::Capture:
        return capture(step.capture, error);
    }
    return true;
}

QWidget* ShotRunner::findWidget(const QString& objectName, QString* error) const {
    for (QWidget* top : QApplication::topLevelWidgets()) {
        if (top->objectName() == objectName) return top;
        if (auto* child = top->findChild<QWidget*>(objectName)) return child;
    }
    *error = QStringLiteral("no widget named '%1'").arg(objectName);
    return nullptr;
}

QAction* ShotRunner::findMenuAction(const QString& path, QString* error) const {
    const QStringList parts = menuPath(path);
    QList<QAction*> actions = m_window->menuBar()->actions();
    QAction* found = nullptr;
    for (int level = 0; level < parts.size(); ++level) {
        found = nullptr;
        for (QAction* a : actions) {
            if (!a->isSeparator() && normalizedMenuText(a->text()) == parts[level]) {
                found = a;
                break;
            }
        }
        if (!found) {
            QStringList known;
            for (QAction* a : actions)
                if (!a->isSeparator()) known << normalizedMenuText(a->text());
            *error = QStringLiteral("menu path '%1': no '%2' (has: %3)")
                         .arg(path, parts[level], known.join(QStringLiteral(", ")));
            return nullptr;
        }
        if (level + 1 < parts.size()) {
            if (!found->menu()) {
                *error = QStringLiteral("menu path '%1': '%2' has no submenu").arg(path, parts[level]);
                return nullptr;
            }
            actions = found->menu()->actions();
        }
    }
    return found;
}

bool ShotRunner::openMenu(const QString& path, std::function<void()>* deferred, QString* error) {
    const QStringList parts = menuPath(path);
    // Validate the whole chain up front: each level must be a submenu.
    QList<QAction*> chain;
    for (int level = 1; level <= parts.size(); ++level) {
        QAction* a = findMenuAction(QStringList(parts.mid(0, level)).join(QLatin1Char('>')), error);
        if (!a) return false;
        if (!a->menu()) {
            *error = QStringLiteral("'%1' is a menu item, not a menu -- use 'action:' to trigger it").arg(path);
            return false;
        }
        chain << a;
    }

    QMenuBar* bar = m_window->menuBar();
    if (bar->isNativeMenuBar()) {
#ifdef Q_OS_MACOS
        // Native menu-bar menus aren't Qt widgets: open them the way a user
        // would, through the Accessibility API (System Events). Detached,
        // since osascript only returns once the menu closes again.
        QString target = QStringLiteral("menu bar item %1 of menu bar 1").arg(appleScriptString(parts[0]));
        QString script = QStringLiteral("tell application \"System Events\" to tell (first process whose unix id is %1)\n")
                             .arg(QCoreApplication::applicationPid());
        // The menu bar belongs to the frontmost app, and macOS may refuse a
        // terminal-launched app's own activation request -- bring it forward
        // through System Events, then wait for its item (up to 5 s).
        script += QStringLiteral("  set frontmost to true\n");
        script += QStringLiteral("  repeat 20 times\n    if exists %1 then exit repeat\n    delay 0.25\n"
                                 "  end repeat\n")
                      .arg(target);
        script += QStringLiteral("  click %1\n").arg(target);
        for (int level = 1; level < parts.size(); ++level) {
            target = QStringLiteral("menu item %1 of menu 1 of %2").arg(appleScriptString(parts[level]), target);
            script += QStringLiteral("  delay 0.3\n  click %1\n").arg(target);
        }
        script += QStringLiteral("end tell\n");
        m_nativeMenuDepth = static_cast<int>(parts.size());
        if (m_menuScript) m_menuScript->deleteLater();
        m_menuScript = new QProcess(this);
        *deferred = [process = m_menuScript, script] {
            process->start(QStringLiteral("/usr/bin/osascript"), {QStringLiteral("-e"), script});
        };
        return true;
#else
        *error = QStringLiteral("native menu bars can only be opened on macOS");
        return false;
#endif
    }

    // Qt-drawn menu bar: open the chain in-process, so a `method: qt`
    // capture of the window shows it.
    *deferred = [bar, chain] {
        bar->setActiveAction(chain[0]);
        QMenu* parent = chain[0]->menu();
        for (int level = 1; level < chain.size(); ++level) {
            parent->setActiveAction(chain[level]);
            const QRect item = parent->actionGeometry(chain[level]);
            chain[level]->menu()->popup(parent->mapToGlobal(item.topRight()));
            parent = chain[level]->menu();
        }
    };
    return true;
}

bool ShotRunner::closeTopmost(QString* error) {
    if (m_nativeMenuDepth > 0) {
#ifdef Q_OS_MACOS
        const QString script =
            QStringLiteral("tell application \"System Events\"\n  repeat %1 times\n    key code 53\n"
                           "    delay 0.1\n  end repeat\nend tell\n")
                .arg(m_nativeMenuDepth);
        QProcess::startDetached(QStringLiteral("/usr/bin/osascript"), {QStringLiteral("-e"), script});
#endif
        m_nativeMenuDepth = 0;
        return true;
    }
    if (QWidget* popup = QApplication::activePopupWidget()) {
        popup->close();
        return true;
    }
    if (QDialog* dialog = topmostDialog()) {
        dialog->reject();
        return true;
    }
    if (QWidget* modal = QApplication::activeModalWidget()) {
        modal->close();
        return true;
    }
    *error = QStringLiteral("nothing to close");
    return false;
}

void ShotRunner::closeAllTransient() {
    QString ignored;
    if (m_nativeMenuDepth > 0) closeTopmost(&ignored);
    for (int i = 0; i < 20; ++i) {
        if (!QApplication::activePopupWidget() && !topmostDialog() && !QApplication::activeModalWidget()) break;
        closeTopmost(&ignored);
    }
}

bool ShotRunner::capture(const ShotCaptureSpec& spec, QString* error) {
    const QString file = QDir(m_outDir).absoluteFilePath(spec.file);
    QCoreApplication::processEvents(); // pending layout/paint from the last step
    // Screenshots normally show RUN mode; PRO is only right for LIST or
    // program entry, so point it out rather than fail.
    for (const auto& [symbol, on] : m_window->controller()->currentDisplay().statusSymbols) {
        if (on && symbol == "PRO") log(QStringLiteral("  note: the calculator is in PRO mode"));
    }
    // The window can't go below its layout's minimum (the PC-1600 control
    // bar is wide) or past the screen -- say so, the framing is then not
    // what the scenario asked for.
    const QSize wanted = wantedWindowSize();
    if (wanted.isValid() && m_window->size() != wanted)
        log(QStringLiteral("  note: window is %1x%2, not the %3x%4 asked for")
                .arg(m_window->width())
                .arg(m_window->height())
                .arg(wanted.width())
                .arg(wanted.height()));

    using Target = ShotCaptureSpec::Target;
    QWidget* target = nullptr;
    switch (spec.target) {
        case Target::Window:
        case Target::ScreenRegion:
            target = m_window;
            break;
        case Target::Dialog:
            target = topmostDialog();
            if (!target) {
                *error = QStringLiteral("no dialog is open");
                return false;
            }
            break;
        case Target::LcdImage: {
            // What Edit > Copy Screen puts on the clipboard: the dot matrix at
            // its physical size (see MainWindow::copyScreenToClipboard()).
            const QImage image = MainWindow::toQImage(m_window->controller()->currentScreenImage());
            if (!ShotCapture::savePng(image, file, error)) return false;
            break;
        }
        case Target::Plot:
            if (!ShotCapture::savePng(m_window->plotterPaper()->renderPaperImage(), file, error)) {
                if (error->startsWith(QLatin1String("nothing"))) *error = QStringLiteral("the plotter paper is blank");
                return false;
            }
            break;
        case Target::Widget:
            target = findWidget(spec.objectName, error);
            if (!target) return false;
            if (!target->isVisible()) {
                *error = QStringLiteral("'%1' is not visible").arg(spec.objectName);
                return false;
            }
            break;
    }

    if (target) {
        QList<QWidget*> extras = ShotCapture::visibleTransients(m_window);
        extras.removeAll(target);
        extras.removeAll(target->window()); // a section of a dialog: not the whole dialog
        bool ok = false;
        if (spec.method == ShotCaptureSpec::Method::Qt) {
            ok = ShotCapture::savePng(ShotCapture::renderComposite(target, extras, spec.padding, spec.scale), file,
                                      error);
        } else if (spec.target == Target::Window || spec.target == Target::Dialog) {
            ok = ShotCapture::systemCaptureWindow(target, file, error);
        } else {
            QRect area;
#ifdef Q_OS_MACOS
            if (spec.target == Target::ScreenRegion) {
                // With a menu-bar menu open: just the open menus, extended
                // up to the top of the screen so their titles in the menu
                // bar are in the picture too. Otherwise every window we own.
                if (m_nativeMenuDepth > 0) {
                    area = macOwnWindowsBounds(/*menusOnly=*/true);
                    if (!area.isEmpty() && m_window->screen())
                        area.setTop(m_window->screen()->geometry().top());
                } else {
                    area = macOwnWindowsBounds();
                }
            }
#endif
            if (area.isEmpty()) area = ShotCapture::compositeRect(target, extras, 0);
            area.adjust(-spec.padding, -spec.padding, spec.padding, spec.padding);
            ok = ShotCapture::systemCaptureRect(area, file, error);
        }
        if (!ok) return false;
    }
    m_written++;
    log(QStringLiteral("  wrote %1").arg(QDir(m_outDir).relativeFilePath(file)));
    return true;
}
