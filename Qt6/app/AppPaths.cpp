#include "AppPaths.hpp"
#include "AppSettings.hpp"

#include <QCoreApplication>
#include <QDir>

namespace AppPaths {

namespace {

// Where the bundled resources live -- *.card.yaml catalog AND ROM images
// (disjoint filename extensions, one shared directory) -- resolved
// relative to the running executable: macOS .app bundle's
// Contents/Resources (via applicationDirPath()'s Contents/MacOS +
// "../Resources"), or a "resources" folder next to the binary on
// Windows/Linux (matching windeployqt's and AppImage's flat-next-to-binary
// convention). Read-only bundled content -- never created if missing; a
// missing/unreadable directory is tolerated by scanMemoryCardDirectory()/
// BundledRomCatalog (empty catalogue / resolve failure).
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

QString q(const std::string& s) { return QString::fromStdString(s); }

} // namespace

shared::AppPaths& paths() {
    static shared::AppPaths p(AppSettings::settings(),
                              shared::HostDirs{QDir::homePath().toStdString(), bundledResourcesDir().toStdString(), {}});
    return p;
}

QString defaultInstanceDir() { return q(paths().defaultInstanceDir()); }
void setIsolatedInstanceDir(const QString& dir) { paths().setIsolatedInstanceDir(dir.toStdString()); }
QString displayPath(const QString& path) { return q(paths().displayPath(path.toStdString())); }
QString instanceDir() { return q(paths().instanceDir()); }
QString tapeDir() { return q(paths().tapeDir()); }
QString traceDir() { return q(paths().traceDir()); }
QString serialLinkDir() { return q(paths().serialLinkDir()); }
}  // namespace AppPaths
