#include "AppPaths.hpp"
#include "AppSettings.hpp"
#include "Connector/FloppyImageFile.hpp"
#include "Connector/MemoryCardCatalog.hpp"

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

namespace {
QString& isolatedInstanceDir() {
    static QString dir;
    return dir;
}

// `path` with the leading `dir` replaced by `shown`, or empty when `path`
// isn't `dir` itself or inside it.
QString replacePrefix(const QString& path, const QString& dir, const QString& shown) {
    if (dir.isEmpty() || !path.startsWith(dir)) return QString();
    if (path.size() > dir.size() && path.at(dir.size()) != QLatin1Char('/')) return QString();
    return shown + path.mid(dir.size());
}
} // namespace

QString defaultInstanceDir() {
    const QString dir = isolatedInstanceDir().isEmpty() ? QDir::homePath() + QStringLiteral("/Calc-U-1600")
                                                        : isolatedInstanceDir();
    QDir().mkpath(dir);
    return dir;
}

void setIsolatedInstanceDir(const QString& dir) {
    isolatedInstanceDir() = dir;
}

QString displayPath(const QString& path) {
    QString shown = replacePrefix(path, defaultInstanceDir(), QStringLiteral("~/Calc-U-1600"));
    if (shown.isEmpty()) shown = replacePrefix(path, QDir::homePath(), QStringLiteral("~"));
    return shown.isEmpty() ? path : shown;
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
    return QString::fromStdString(namedFileName(instanceName.toStdString(), kCardFileSuffix));
}

QString instancePathFor(const QString& instanceName) {
    return QDir(instanceDir()).filePath(sanitizedInstanceFileName(instanceName));
}

QString sanitizedFloppyFileName(const QString& diskName) {
    return QString::fromStdString(namedFileName(diskName.toStdString(), kFloppyFileSuffix));
}

QString floppyInstancePathFor(const QString& diskName) {
    return QDir(instanceDir()).filePath(sanitizedFloppyFileName(diskName));
}

QString tapeDir() {
    QString dir = AppSettings::tapeDirOverride();
    if (dir.isEmpty()) dir = QDir(instanceDir()).filePath(QStringLiteral("Tapes"));
    QDir().mkpath(dir);
    return dir;
}

QString tapePathFor(const QString& tapeName) {
    return QDir(tapeDir()).filePath(QString::fromStdString(namedFileName(tapeName.toStdString(), ".wav")));
}

bool isUnderDir(const QString& path, const QString& dir) {
    if (path.isEmpty() || dir.isEmpty()) return false;
    const QString a = QFileInfo(path).canonicalFilePath();
    const QString b = QFileInfo(dir).canonicalFilePath();
    if (a.isEmpty() || b.isEmpty()) return false;
    // Match whole path components: "<dir>-backup/x" is not under "<dir>".
    return a == b || a.startsWith(b.endsWith(QLatin1Char('/')) ? b : b + QLatin1Char('/'));
}

bool atomicWriteFile(const QString& path, const std::string& text) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(text.data(), static_cast<qint64>(text.size()));
    return f.commit();
}

bool planNamedSave(MediaKind kind, const QString& name, const QString& explicitPath, bool asTemplate,
                   QString* path, QString* error) {
    std::string planned;
    const MediaSaveRefusal refusal =
        planMediaSave(kind, name.toStdString(), explicitPath.toStdString(), asTemplate,
                      {bundledResourcesDir().toStdString(), instanceDir().toStdString()}, &planned);
    *path = QString::fromStdString(planned);
    switch (refusal) {
        case MediaSaveRefusal::None: return true;
        case MediaSaveRefusal::EmptyName:
            *error = QCoreApplication::translate("AppPaths", "Name cannot be empty.");
            break;
        case MediaSaveRefusal::QuoteInName:
            *error = QCoreApplication::translate("AppPaths", "Name cannot contain '\"'.");
            break;
        case MediaSaveRefusal::NoSaveFolder:
            *error = QCoreApplication::translate("AppPaths", "There is no save folder.");
            break;
        case MediaSaveRefusal::TemplateName:
            *error = QCoreApplication::translate("AppPaths", "\"%1\" is a template's name. Choose a different name.")
                         .arg(name);
            break;
        case MediaSaveRefusal::TemplateFile:
            *error = QCoreApplication::translate(
                         "AppPaths", "\"%1\" is a template file and is never overwritten. Choose a different name.")
                         .arg(*path);
            break;
    }
    return false;
}

}  // namespace AppPaths
