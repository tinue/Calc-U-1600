#include "AppPaths.hpp"
#include "AppSettings.hpp"

#include <QCoreApplication>
#include <QDir>

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

shared::AppPaths& paths() {
    static shared::AppPaths p(AppSettings::settings(),
                              shared::HostDirs{QDir::homePath().toStdString(), bundledResourcesDir().toStdString()});
    return p;
}

namespace {
QString q(const std::string& s) { return QString::fromStdString(s); }
} // namespace

QString defaultInstanceDir() { return q(paths().defaultInstanceDir()); }
void setIsolatedInstanceDir(const QString& dir) { paths().setIsolatedInstanceDir(dir.toStdString()); }
QString displayPath(const QString& path) { return q(paths().displayPath(path.toStdString())); }
QString instanceDir() { return q(paths().instanceDir()); }
QString instancePathFor(const QString& instanceName) { return q(paths().instancePathFor(instanceName.toStdString())); }
QString floppyInstancePathFor(const QString& diskName) {
    return q(paths().floppyInstancePathFor(diskName.toStdString()));
}
QString tapeDir() { return q(paths().tapeDir()); }
QString tapePathFor(const QString& tapeName) { return q(paths().tapePathFor(tapeName.toStdString())); }
bool isUnderDir(const QString& path, const QString& dir) {
    return shared::isUnderDir(path.toStdString(), dir.toStdString());
}

bool atomicWriteFile(const QString& path, const std::string& text) {
    return shared::atomicWriteFile(path.toStdString(), text);
}

bool planNamedSave(MediaKind kind, const QString& name, const QString& explicitPath, bool asTemplate,
                   QString* path, QString* error) {
    std::string planned;
    const MediaSaveRefusal refusal =
        paths().planNamedSave(kind, name.toStdString(), explicitPath.toStdString(), asTemplate, &planned);
    *path = QString::fromStdString(planned);
    if (refusal == MediaSaveRefusal::None) return true;
    *error = QString::fromStdString(shared::mediaSaveRefusalMessage(refusal, name.toStdString(), planned));
    return false;
}

}  // namespace AppPaths
