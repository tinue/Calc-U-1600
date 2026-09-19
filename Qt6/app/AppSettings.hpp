#pragma once
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QSize>
#include <QString>

// The QSettings key namespace for the app. Uses the app's own name for
// both organization and application, avoiding an unrelated-looking
// reverse-DNS identifier.
namespace AppSettings {

inline QSettings backingStore() { return QSettings(QStringLiteral("Calc-U-1600"), QStringLiteral("Calc-U-1600")); }

// Key: "storage/instanceDirOverride" -- empty/absent means "use the
// ~/Calc-U-1600 default" (see AppPaths::instanceDir()), not "use
// QStandardPaths" -- there is no AppData/Application Support fallback in
// this design at all.
inline QString instanceDirOverride() {
    return backingStore().value(QStringLiteral("storage/instanceDirOverride"), QString()).toString();
}

inline void setInstanceDirOverride(const QString& dir) {
    QSettings s = backingStore();
    if (dir.isEmpty())
        s.remove(QStringLiteral("storage/instanceDirOverride"));
    else
        s.setValue(QStringLiteral("storage/instanceDirOverride"), dir);
}

// The folders the file-open dialogs start in, one per kind of file, each
// shown as its own row in Settings:
//   Samples  -- "Load Preset…" (and Settings' default-preset pickers)
//   Basic    -- "Load BASIC Program…" (.bas listings)
//   Assembly -- machine-code sources; no dialog uses it yet
enum class OpenFolder { Samples, Basic, Assembly };

// Key group per folder. Samples keeps the original "preset/" keys so a
// folder configured before the split carries over.
inline QString openFolderKeyGroup(OpenFolder folder) {
    switch (folder) {
        case OpenFolder::Samples: return QStringLiteral("preset/");
        case OpenFolder::Basic: return QStringLiteral("basic/");
        case OpenFolder::Assembly: return QStringLiteral("assembly/");
    }
    return QStringLiteral("preset/");
}

// Key: "<group>openDir" -- a fixed folder the dialog starts in, set via
// Settings and not auto-updated by each Open. Empty/absent -- the default,
// and what Settings' Reset restores -- means "<last used>": the dialog
// starts in lastOpenDir() instead.
inline QString openDir(OpenFolder folder) {
    return backingStore().value(openFolderKeyGroup(folder) + QStringLiteral("openDir"), QString()).toString();
}

inline void setOpenDir(OpenFolder folder, const QString& dir) {
    QSettings s = backingStore();
    const QString key = openFolderKeyGroup(folder) + QStringLiteral("openDir");
    if (dir.isEmpty())
        s.remove(key);
    else
        s.setValue(key, dir);
}

// Key: "<group>lastOpenDir" -- the folder of the file last picked in that
// dialog. Recorded on every pick (rememberOpenFile()), but only used while
// openDir() is unset.
inline QString lastOpenDir(OpenFolder folder) {
    return backingStore().value(openFolderKeyGroup(folder) + QStringLiteral("lastOpenDir"), QString()).toString();
}

inline void rememberOpenFile(OpenFolder folder, const QString& filePath) {
    backingStore().setValue(openFolderKeyGroup(folder) + QStringLiteral("lastOpenDir"),
                            QFileInfo(filePath).absolutePath());
}

// The start directory for that dialog: the fixed openDir() if set, else
// the last-used folder, else the user's home directory.
inline QString openStartDir(OpenFolder folder) {
    QString dir = openDir(folder);
    if (dir.isEmpty()) dir = lastOpenDir(folder);
    return dir.isEmpty() ? QDir::homePath() : dir;
}

// Key: "preset/default/<model>" -- `modelKey` is "PC1500", "PC1500A" or
// "PC1600" (the same strings startupModelPreference() stores). The preset
// file applied whenever that model gets selected; empty/absent means none
// (the model just boots bare).
inline QString defaultPresetPath(const QString& modelKey) {
    return backingStore().value(QStringLiteral("preset/default/") + modelKey, QString()).toString();
}

inline void setDefaultPresetPath(const QString& modelKey, const QString& path) {
    QSettings s = backingStore();
    if (path.isEmpty())
        s.remove(QStringLiteral("preset/default/") + modelKey);
    else
        s.setValue(QStringLiteral("preset/default/") + modelKey, path);
}

// Key: "trace/dirOverride" -- empty/absent means "write TRACE.bin under
// AppPaths::instanceDir()" (DebugPanel's default), matching
// instanceDirOverride()'s own empty-means-default convention.
inline QString traceDirOverride() {
    return backingStore().value(QStringLiteral("trace/dirOverride"), QString()).toString();
}

inline void setTraceDirOverride(const QString& dir) {
    QSettings s = backingStore();
    if (dir.isEmpty())
        s.remove(QStringLiteral("trace/dirOverride"));
    else
        s.setValue(QStringLiteral("trace/dirOverride"), dir);
}

// Key: "trace/maxFileSizeMB" -- the cap DebugPanel's trace writer rolls
// TRACE.bin over at, in megabytes.
inline int traceMaxFileSizeMB() {
    return backingStore().value(QStringLiteral("trace/maxFileSizeMB"), 50).toInt();
}

inline void setTraceMaxFileSizeMB(int mb) {
    backingStore().setValue(QStringLiteral("trace/maxFileSizeMB"), mb);
}

// Key: "serial/linkDirectory" -- directory PtySerialLink creates its stable
// `calcu1600.serial` symlink in. Empty/absent means AppPaths::instanceDir()
// (the same ~/Calc-U-1600 default everything else uses); MachineController
// always resolves and passes a concrete directory here.
inline QString serialLinkDirOverride() {
    return backingStore().value(QStringLiteral("serial/linkDirectory"), QString()).toString();
}

inline void setSerialLinkDirOverride(const QString& dir) {
    QSettings s = backingStore();
    if (dir.isEmpty())
        s.remove(QStringLiteral("serial/linkDirectory"));
    else
        s.setValue(QStringLiteral("serial/linkDirectory"), dir);
}

// Key: "window/width" / "window/height" -- last user-resized main-window
// size, restored on the next launch instead of always opening at the
// original hardcoded 900x500 default.
inline QSize windowSize() {
    QSettings s = backingStore();
    const int w = s.value(QStringLiteral("window/width"), 900).toInt();
    const int h = s.value(QStringLiteral("window/height"), 500).toInt();
    return QSize(w, h);
}

inline void setWindowSize(const QSize& size) {
    QSettings s = backingStore();
    s.setValue(QStringLiteral("window/width"), size.width());
    s.setValue(QStringLiteral("window/height"), size.height());
}

// Key: "startup/modelPreference" -- which model to boot into: "last" (the
// default) reuses lastUsedModel() below, or one of "PC1500"/"PC1500A"/
// "PC1600" pins a specific model regardless of what was last selected.
// There is deliberately no equivalent ROM-revision preference: the plain
// PC-1500 always boots on ROM A04 (MachineController's own default), and
// that isn't user-configurable.
inline QString startupModelPreference() {
    return backingStore().value(QStringLiteral("startup/modelPreference"), QStringLiteral("last")).toString();
}

inline void setStartupModelPreference(const QString& pref) {
    backingStore().setValue(QStringLiteral("startup/modelPreference"), pref);
}

// Key: "startup/lastUsedModel" -- the Model enum value (as int) of whatever
// was last switched to, updated on every MachineController::switchModel()
// call. Only consulted at startup when startupModelPreference() == "last".
// Default 1 == Model::PC1500A, MachineController's default model.
inline int lastUsedModel() {
    return backingStore().value(QStringLiteral("startup/lastUsedModel"), 1).toInt();
}

inline void setLastUsedModel(int model) {
    backingStore().setValue(QStringLiteral("startup/lastUsedModel"), model);
}

}  // namespace AppSettings
