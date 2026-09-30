#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileOpenEvent>
#include <QIcon>
#include <QTemporaryDir>
#include <cstdio>
#include <functional>
#include <utility>
#include "AppLogging.hpp"
#include "AppPaths.hpp"
#include "AppSettings.hpp"
#ifdef __APPLE__
#include "MacAppSupport.h"
#endif
#include "MainWindow.hpp"
#include "debug/DebugController.hpp"
#include "screenshots/ShotRunner.hpp"
#include "screenshots/ShotScenario.hpp"

namespace {

// macOS hands a file dropped onto the Dock icon, or opened from Finder, to
// the application object as a QFileOpenEvent -- at a cold launch before
// there is a window. Kept until the window takes them (openDroppedFile()).
class Application : public QApplication {
public:
    using QApplication::QApplication;

    void setFileOpenHandler(std::function<void(const QString&)> handler) {
        m_onFileOpen = std::move(handler);
        for (const QString& path : std::exchange(m_pending, {})) m_onFileOpen(path);
    }

protected:
    bool event(QEvent* event) override {
        if (event->type() != QEvent::FileOpen) return QApplication::event(event);
        const QString path = static_cast<QFileOpenEvent*>(event)->file();
        if (m_onFileOpen)
            m_onFileOpen(path);
        else
            m_pending.append(path);
        return true;
    }

private:
    std::function<void(const QString&)> m_onFileOpen;
    QStringList m_pending;
};

}  // namespace

int main(int argc, char** argv) {
    AppLogging::install();
#ifdef __APPLE__
    macDisableWindowRestoration(); // see MacAppSupport.h: the restore prompt deadlocks the startup preset
    macDisablePressAndHold();      // see MacAppSupport.h: held letters stay held keys
#endif
    Application app(argc, argv);
    // Without this, the running window's title-bar/taskbar icon is
    // whatever the platform defaults to (e.g. a generic AppImage icon on
    // Linux) -- the .desktop file's Icon= only covers desktop-environment
    // integration (launcher/file-manager icon), not the live window.
    // Not on macOS: there it would replace the bundle's system-masked
    // Dock icon (AppIcon.icon) with this full-bleed PNG.
#ifndef __APPLE__
    app.setWindowIcon(QIcon(":/app/icon.png"));
#endif

    // Scripted screenshots (docs/developer/screenshots/README.md): play a
    // scenario, write its images, quit with 0 (all written) or 1 (any failed).
    QCommandLineParser parser;
    const QCommandLineOption shotsOption(QStringLiteral("shots"),
                                         QStringLiteral("Run a screenshot scenario (*.shots.yaml), then quit."),
                                         QStringLiteral("scenario"));
    const QCommandLineOption shotsOutOption(QStringLiteral("shots-out"),
                                            QStringLiteral("Write the images here instead of the scenario's 'out:'."),
                                            QStringLiteral("dir"));
    const QCommandLineOption shotsOnlyOption(QStringLiteral("shots-only"),
                                             QStringLiteral("Only run these shots (comma-separated names)."),
                                             QStringLiteral("names"));
    const QCommandLineOption shotsFailFastOption(QStringLiteral("shots-fail-fast"),
                                                 QStringLiteral("Stop at the first failing shot."));
    const QCommandLineOption dapOption(QStringLiteral("dap"),
                                       QStringLiteral("Accept a debugger on 127.0.0.1:<port> for this run (Settings unchanged)."),
                                       QStringLiteral("port"));
    parser.addOptions({shotsOption, shotsOutOption, shotsOnlyOption, shotsFailFastOption, dapOption});
    // parse(), not process(): a normal launch must survive whatever extra
    // arguments macOS or an IDE add (`-NSDocumentRevisionsDebugMode YES`).
    const bool parsed = parser.parse(QCoreApplication::arguments());

    if (parser.isSet(dapOption)) DebugController::setCommandLinePort(parser.value(dapOption).toInt());

    if (!parser.isSet(shotsOption)) {
        MainWindow window;
        window.show();
        app.setFileOpenHandler([&window](const QString& path) { window.openDroppedFile(path); });
        const int result = app.exec();
        app.setFileOpenHandler({});
        return result;
    }

    if (!parsed) {
        std::fprintf(stderr, "[shots] %s\n", qPrintable(parser.errorText()));
        return 2;
    }
    ShotScenario scenario;
    QString error;
    if (!parseShotScenario(parser.value(shotsOption), &scenario, &error)) {
        std::fprintf(stderr, "[shots] %s\n", qPrintable(error));
        return 2;
    }
    // A throwaway settings store and card/disk folder: the user's own
    // settings (default presets, last model, folders, saved cards) must not
    // show up in the images, and the run must not change them.
    QTemporaryDir isolated;
    if (!isolated.isValid()) {
        std::fprintf(stderr, "[shots] could not create a temporary directory\n");
        return 2;
    }
    AppSettings::isolatedStorePath() = isolated.filePath(QStringLiteral("settings.ini"));
    AppPaths::setIsolatedInstanceDir(isolated.filePath(QStringLiteral("instances")));
    // Qt's own file/color dialogs are widgets the script can capture and
    // close; the native ones are not.
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);

    ShotRunner::Options options;
    options.outDirOverride = parser.value(shotsOutOption);
    if (parser.isSet(shotsOnlyOption))
        options.only = parser.value(shotsOnlyOption).split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (QString& name : options.only) name = name.trimmed();
    options.failFast = parser.isSet(shotsFailFastOption);

    MainWindow window;
    ShotRunner runner(&window, scenario, options);
    QObject::connect(&runner, &ShotRunner::finished, &app, &QCoreApplication::exit);
    window.show();
    runner.start();
    return app.exec();
}
