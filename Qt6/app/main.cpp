#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
#include <QTemporaryDir>
#include <cstdio>
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

int main(int argc, char** argv) {
    AppLogging::install();
#ifdef __APPLE__
    macDisableWindowRestoration(); // see MacAppSupport.h: the restore prompt deadlocks the startup preset
#endif
    QApplication app(argc, argv);
    // Without this, the running window's title-bar/taskbar icon is
    // whatever the platform defaults to (e.g. a generic AppImage icon on
    // Linux) -- the .desktop file's Icon= only covers desktop-environment
    // integration (launcher/file-manager icon), not the live window.
    app.setWindowIcon(QIcon(":/app/icon.png"));

    // Scripted screenshots (docs/screenshots/README.md): play a scenario,
    // write its images, quit with 0 (all written) or 1 (any failed).
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
        return app.exec();
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
