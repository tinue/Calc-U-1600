#pragma once
#include <optional>
#include <string>

#include "Shared/MachineModel.hpp"

// ── Settings: key names, defaults and resolution ─────────────────────────
//
// Every setting the app keeps, by key, with its default and the rules for
// reading it back. The frontend only supplies the storage (SettingsStore:
// QSettings in the Qt app, localStorage in a browser). Values are stored
// as strings; numbers in decimal, booleans as "true"/"false".
namespace shared {

class SettingsStore {
public:
    virtual ~SettingsStore() = default;
    virtual std::optional<std::string> get(const std::string& key) const = 0;
    virtual void set(const std::string& key, const std::string& value) = 0;
    virtual void remove(const std::string& key) = 0;
};

// The folders the file-open dialogs start in, one per kind of file, each
// shown as its own row in Settings:
//   Samples   -- "Load Preset…" (and Settings' default-preset pickers)
//   Basic     -- "Load BASIC Program…" (.bas listings)
//   Assembly  -- "Load Machine Code…" (.bin files)
//   HostDrive -- "Mount Directory…" (PC-1600 host drive S3:)
enum class OpenFolder { Samples, Basic, Assembly, HostDrive };

class Settings {
public:
    explicit Settings(SettingsStore& store) : m_store(store) {}

    // "storage/instanceDirOverride" -- empty means "use the ~/Calc-U-1600
    // default" (AppPaths::instanceDir()); there is no AppData/Application
    // Support fallback in this design at all.
    std::string instanceDirOverride() const { return str("storage/instanceDirOverride"); }
    void setInstanceDirOverride(const std::string& dir) { setOrRemove("storage/instanceDirOverride", dir); }

    // "storage/tapeDirOverride" -- the cassette tapes' folder; empty means
    // "Tapes" inside the save folder (AppPaths::tapeDir()).
    std::string tapeDirOverride() const { return str("storage/tapeDirOverride"); }
    void setTapeDirOverride(const std::string& dir) { setOrRemove("storage/tapeDirOverride", dir); }

    // "<group>openDir" -- a fixed folder the dialog starts in, set via
    // Settings and not auto-updated by each Open. Empty -- the default, and
    // what Settings' Reset restores -- means "<last used>": the dialog
    // starts in lastOpenDir() instead.
    std::string openDir(OpenFolder folder) const { return str(group(folder) + "openDir"); }
    void setOpenDir(OpenFolder folder, const std::string& dir) { setOrRemove(group(folder) + "openDir", dir); }

    // "<group>lastOpenDir" -- the folder of the file last picked in that
    // dialog (for HostDrive: the directory last mounted). Recorded on every
    // pick, but only used while openDir() is unset. Stored absolute.
    std::string lastOpenDir(OpenFolder folder) const { return str(group(folder) + "lastOpenDir"); }
    void rememberOpenDir(OpenFolder folder, const std::string& dir);
    void rememberOpenFile(OpenFolder folder, const std::string& filePath);

    // The start directory for that dialog: the fixed openDir() if set, else
    // the last-used folder, else `homeDir`.
    std::string openStartDir(OpenFolder folder, const std::string& homeDir) const;

    // "preset/default/<model>" -- the preset applied whenever that model
    // gets selected; empty means none (the model just boots bare).
    std::string defaultPresetPath(Model model) const { return str(std::string("preset/default/") + modelKey(model)); }
    void setDefaultPresetPath(Model model, const std::string& path) {
        setOrRemove(std::string("preset/default/") + modelKey(model), path);
    }

    // "trace/dirOverride" -- empty means "write TRACE.bin under
    // AppPaths::instanceDir()".
    std::string traceDirOverride() const { return str("trace/dirOverride"); }
    void setTraceDirOverride(const std::string& dir) { setOrRemove("trace/dirOverride", dir); }

    // "trace/maxFileSizeMB" -- the size the trace writer rolls TRACE.bin
    // over at, in megabytes.
    static constexpr int kDefaultTraceMaxFileSizeMB = 50;
    int traceMaxFileSizeMB() const { return integer("trace/maxFileSizeMB", kDefaultTraceMaxFileSizeMB); }
    void setTraceMaxFileSizeMB(int mb) { m_store.set("trace/maxFileSizeMB", std::to_string(mb)); }

    // "debug/dapEnabled" (default off) and "debug/dapPort" (default 32168,
    // anything outside 1024-65535 reads as the default) -- the Debug
    // Adapter Protocol server VS Code attaches to. It listens on 127.0.0.1
    // only.
    static constexpr int kDefaultDapPort = 32168;
    bool dapEnabled() const { return boolean("debug/dapEnabled", false); }
    void setDapEnabled(bool on) { m_store.set("debug/dapEnabled", on ? "true" : "false"); }
    int dapPort() const;
    void setDapPort(int port) { m_store.set("debug/dapPort", std::to_string(port)); }

    // "serial/linkDirectory" -- directory the serial ports' stable symlinks
    // (`calcu1600-rs232c.serial`, ...) go in. Empty means
    // AppPaths::instanceDir().
    std::string serialLinkDirOverride() const { return str("serial/linkDirectory"); }
    void setSerialLinkDirOverride(const std::string& dir) { setOrRemove("serial/linkDirectory", dir); }

    // "window/width" / "window/height" -- last user-resized main-window
    // size, restored on the next launch (default 900x500).
    static constexpr int kDefaultWindowWidth = 900;
    static constexpr int kDefaultWindowHeight = 500;
    int windowWidth() const { return integer("window/width", kDefaultWindowWidth); }
    int windowHeight() const { return integer("window/height", kDefaultWindowHeight); }
    void setWindowSize(int width, int height) {
        m_store.set("window/width", std::to_string(width));
        m_store.set("window/height", std::to_string(height));
    }

    // "startup/modelPreference" -- which model to boot into: "last" (the
    // default) reuses lastUsedModel(), or one of "PC1500"/"PC1500A"/"PC1600"
    // pins a model regardless of what was last selected. There is
    // deliberately no ROM-revision preference: the plain PC-1500 always
    // boots on ROM A04.
    std::string startupModelPreference() const { return str("startup/modelPreference", "last"); }
    void setStartupModelPreference(const std::string& pref) { m_store.set("startup/modelPreference", pref); }

    // "startup/lastUsedModel" -- the Model enum value (as int) last switched
    // to. Only consulted when startupModelPreference() is "last". Default
    // 1 == Model::PC1500A.
    int lastUsedModel() const { return integer("startup/lastUsedModel", 1); }
    void setLastUsedModel(int model) { m_store.set("startup/lastUsedModel", std::to_string(model)); }

    // The model to boot into: the pinned preference if it names a model,
    // else the last used one (an out-of-range stored value falls back to
    // the PC-1500A).
    Model startupModel() const;

private:
    SettingsStore& m_store;

    // Samples keeps the original "preset/" keys so a folder configured
    // before the per-kind split carries over.
    static std::string group(OpenFolder folder);
    std::string str(const std::string& key, const std::string& fallback = {}) const;
    int integer(const std::string& key, int fallback) const;
    bool boolean(const std::string& key, bool fallback) const;
    void setOrRemove(const std::string& key, const std::string& value);
};

} // namespace shared
