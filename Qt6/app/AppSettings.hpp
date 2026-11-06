#pragma once
#include <QDir>
#include <QSettings>
#include <QSize>
#include <QString>

#include "Shared/Settings.hpp"

// The app's settings: key names, defaults and resolution live in
// Shared/Settings.hpp; this file supplies the QSettings storage and keeps
// the QString-typed calls the widgets use. The QSettings namespace uses the
// app's own name for both organization and application, avoiding an
// unrelated-looking reverse-DNS identifier.
namespace AppSettings {

// Non-empty: an INI file every read/write goes to instead of the user's
// real settings. Set once at startup for scripted screenshot runs
// (`--shots`, see main.cpp) so the user's default presets, last model,
// folders etc. neither leak into the images nor get overwritten.
inline QString& isolatedStorePath() {
    static QString path;
    return path;
}

inline QSettings backingStore() {
    if (!isolatedStorePath().isEmpty()) return QSettings(isolatedStorePath(), QSettings::IniFormat);
    return QSettings(QStringLiteral("Calc-U-1600"), QStringLiteral("Calc-U-1600"));
}

// shared::SettingsStore over QSettings. Opens the backing store per call,
// so a switch to isolatedStorePath() takes effect immediately.
class QtSettingsStore : public shared::SettingsStore {
public:
    std::optional<std::string> get(const std::string& key) const override {
        const QVariant v = backingStore().value(QString::fromStdString(key));
        if (!v.isValid()) return std::nullopt;
        return v.toString().toStdString();
    }
    void set(const std::string& key, const std::string& value) override {
        backingStore().setValue(QString::fromStdString(key), QString::fromStdString(value));
    }
    void remove(const std::string& key) override { backingStore().remove(QString::fromStdString(key)); }
};

inline shared::Settings& settings() {
    static QtSettingsStore store;
    static shared::Settings s(store);
    return s;
}

inline QString q(const std::string& s) { return QString::fromStdString(s); }
inline std::string s(const QString& q) { return q.toStdString(); }

inline QString instanceDirOverride() { return q(settings().instanceDirOverride()); }
inline void setInstanceDirOverride(const QString& dir) { settings().setInstanceDirOverride(s(dir)); }

inline QString tapeDirOverride() { return q(settings().tapeDirOverride()); }
inline void setTapeDirOverride(const QString& dir) { settings().setTapeDirOverride(s(dir)); }

using shared::OpenFolder;
inline QString openDir(OpenFolder folder) { return q(settings().openDir(folder)); }
inline void setOpenDir(OpenFolder folder, const QString& dir) { settings().setOpenDir(folder, s(dir)); }
inline void rememberOpenDir(OpenFolder folder, const QString& dir) { settings().rememberOpenDir(folder, s(dir)); }
inline void rememberOpenFile(OpenFolder folder, const QString& filePath) {
    settings().rememberOpenFile(folder, s(filePath));
}
inline QString openStartDir(OpenFolder folder) { return q(settings().openStartDir(folder, s(QDir::homePath()))); }

inline QString defaultPresetPath(shared::Model model) { return q(settings().defaultPresetPath(model)); }
inline void setDefaultPresetPath(shared::Model model, const QString& path) {
    settings().setDefaultPresetPath(model, s(path));
}

inline QString traceDirOverride() { return q(settings().traceDirOverride()); }
inline void setTraceDirOverride(const QString& dir) { settings().setTraceDirOverride(s(dir)); }
inline int traceMaxFileSizeMB() { return settings().traceMaxFileSizeMB(); }
inline void setTraceMaxFileSizeMB(int mb) { settings().setTraceMaxFileSizeMB(mb); }

inline bool dapEnabled() { return settings().dapEnabled(); }
inline void setDapEnabled(bool on) { settings().setDapEnabled(on); }
inline int dapPort() { return settings().dapPort(); }
inline void setDapPort(int port) { settings().setDapPort(port); }

inline QString serialLinkDirOverride() { return q(settings().serialLinkDirOverride()); }
inline void setSerialLinkDirOverride(const QString& dir) { settings().setSerialLinkDirOverride(s(dir)); }

inline QSize windowSize() { return QSize(settings().windowWidth(), settings().windowHeight()); }
inline void setWindowSize(const QSize& size) { settings().setWindowSize(size.width(), size.height()); }

inline QString startupModelPreference() { return q(settings().startupModelPreference()); }
inline void setStartupModelPreference(const QString& pref) { settings().setStartupModelPreference(s(pref)); }

}  // namespace AppSettings
