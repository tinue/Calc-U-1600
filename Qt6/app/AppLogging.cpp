#include "AppLogging.hpp"
#include "AppPaths.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QtGlobal>

#if defined(Q_OS_LINUX)
#include <syslog.h>
#endif

namespace AppLogging {

namespace {

const char* levelName(QtMsgType type) {
    switch (type) {
        case QtDebugMsg: return "DEBUG";
        case QtInfoMsg: return "INFO";
        case QtWarningMsg: return "WARNING";
        case QtCriticalMsg: return "CRITICAL";
        case QtFatalMsg: return "FATAL";
    }
    return "DEBUG";
}

#if defined(Q_OS_WIN)
void windowsFileHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
    Q_UNUSED(context);
    // Reuses AppPaths' existing ~/Calc-U-1600 directory (already created
    // for battery-card instances) rather than introducing a second,
    // Windows-only app-data location just for logs.
    static const QString logPath =
        AppPaths::defaultInstanceDir() + QStringLiteral("/Calc-U-1600.log");
    QFile f(logPath);
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    QTextStream out(&f);
    out << QDateTime::currentDateTime().toString(Qt::ISODate) << " [" << levelName(type) << "] "
        << msg << '\n';
}
#elif defined(Q_OS_LINUX)
void syslogHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
    Q_UNUSED(context);
    int priority = LOG_DEBUG;
    switch (type) {
        case QtDebugMsg: priority = LOG_DEBUG; break;
        case QtInfoMsg: priority = LOG_INFO; break;
        case QtWarningMsg: priority = LOG_WARNING; break;
        case QtCriticalMsg: priority = LOG_ERR; break;
        case QtFatalMsg: priority = LOG_CRIT; break;
    }
    syslog(priority, "%s", qUtf8Printable(msg));
}
#endif

}  // namespace

void install() {
#if defined(Q_OS_WIN)
    QDir().mkpath(AppPaths::defaultInstanceDir());
    qInstallMessageHandler(windowsFileHandler);
#elif defined(Q_OS_LINUX)
    openlog("Calc-U-1600", LOG_PID, LOG_USER);
    qInstallMessageHandler(syslogHandler);
#endif
}

}  // namespace AppLogging
