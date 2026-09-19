#include "AppPaths.hpp"
#include "AppSettings.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace AppPaths {

QString bundledResourcesDir() {
    const QString exeDir = QCoreApplication::applicationDirPath();
#if defined(Q_OS_MACOS)
    // applicationDirPath() == .../Calc-U-1600.app/Contents/MacOS
    return QDir::cleanPath(QDir(exeDir).filePath(QStringLiteral("../Resources")));
#else
    // Windows: exeDir/resources (windeployqt lays extra files flat next
    // to the .exe by convention). Linux/AppImage: exeDir/resources --
    // AppRun launches the real binary from inside the AppDir, and a
    // "resources" dir placed next to it (see CMakeLists.txt's install()
    // rule) keeps this relative lookup correct whether running from a
    // raw build dir, an installed prefix, or a mounted AppImage.
    return QDir(exeDir).filePath(QStringLiteral("resources"));
#endif
}

QString defaultInstanceDir() {
    const QString dir = QDir::homePath() + QStringLiteral("/Calc-U-1600");
    QDir().mkpath(dir);
    return dir;
}

QString instanceDir() {
    const QString override = AppSettings::instanceDirOverride();
    if (!override.isEmpty()) {
        QDir().mkpath(override);
        return override;
    }
    return defaultInstanceDir();
}

QString sanitizedInstanceFileName(const QString& instanceName) {
    QString s = instanceName;
    s.replace('/', '-').replace(':', '-');
    s = s.simplified();  // trims + collapses internal whitespace runs
    if (s.isEmpty()) s = QStringLiteral("Untitled");
    return s + QStringLiteral(".card.yaml");
}

QString instancePathFor(const QString& instanceName) {
    return QDir(instanceDir()).filePath(sanitizedInstanceFileName(instanceName));
}

QString sanitizedFloppyFileName(const QString& diskName) {
    QString s = diskName;
    s.replace('/', '-').replace(':', '-');
    s = s.simplified();
    if (s.isEmpty()) s = QStringLiteral("Untitled");
    return s + QStringLiteral(".floppy.yaml");
}

QString floppyInstancePathFor(const QString& diskName) {
    return QDir(instanceDir()).filePath(sanitizedFloppyFileName(diskName));
}

bool isUnderDir(const QString& path, const QString& dir) {
    if (path.isEmpty() || dir.isEmpty()) return false;
    const QString a = QFileInfo(path).canonicalFilePath();
    const QString b = QFileInfo(dir).canonicalFilePath();
    return !a.isEmpty() && !b.isEmpty() && a.startsWith(b);
}

bool atomicWriteFile(const QString& path, const std::string& text) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(text.data(), static_cast<qint64>(text.size()));
    return f.commit();
}

}  // namespace AppPaths
