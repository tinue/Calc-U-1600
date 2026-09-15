#pragma once
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

// Key: "preset/openDir" -- the folder both the "Load Preset…" and "Load
// BASIC Program…" file dialogs start in (labeled "Default samples folder"
// in Settings, since preset files and bare .bas listings both live there
// in practice). Unlike instanceDirOverride, this has no environment-
// derived default at all: empty/absent just means "let QFileDialog pick"
// (its own last-visited-directory recall). A configured value here is
// fixed (set via Settings), not auto-updated by each Open -- picking a
// file elsewhere doesn't silently change it.
inline QString presetOpenDir() {
    return backingStore().value(QStringLiteral("preset/openDir"), QString()).toString();
}

inline void setPresetOpenDir(const QString& dir) {
    QSettings s = backingStore();
    if (dir.isEmpty())
        s.remove(QStringLiteral("preset/openDir"));
    else
        s.setValue(QStringLiteral("preset/openDir"), dir);
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
