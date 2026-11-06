#pragma once
#include <string>

#include "Connector/MediaSave.hpp"
#include "Shared/Settings.hpp"

// ── AppPaths: where the app's files live ─────────────────────────────────
//
// The save folder (~/Calc-U-1600 unless overridden in Settings), the
// tapes folder, the file a by-name save of a card, disk or tape goes to,
// and how a path is shown to the user. The frontend supplies the two
// host-specific directories (HostDirs): the home folder and the bundled
// resources (*.card.yaml / *.floppy.yaml templates and ROM images, read
// only). Folders the app writes to are created when asked for.
namespace shared {

struct HostDirs {
    std::string home;
    std::string bundledResources;
};

class AppPaths {
public:
    AppPaths(const Settings& settings, HostDirs dirs) : m_settings(settings), m_dirs(std::move(dirs)) {}

    const std::string& bundledResourcesDir() const { return m_dirs.bundledResources; }
    const std::string& homeDir() const { return m_dirs.home; }

    // Screenshot runs make a throwaway folder the default storage root, so
    // the user's saved cards/disks stay out of the pictures and untouched.
    // Not a setting.
    void setIsolatedInstanceDir(const std::string& dir) { m_isolatedInstanceDir = dir; }

    // ~/Calc-U-1600 (home dir, non-dotted) -- or the isolated folder --
    // created if missing.
    std::string defaultInstanceDir() const;

    // Settings::instanceDirOverride() if set, else defaultInstanceDir().
    // Where battery-card instances and saved disks live right now.
    std::string instanceDir() const;

    // `path` the way every label shows one: under defaultInstanceDir() as
    // "~/Calc-U-1600/...", else under the home folder as "~/...", else as
    // is. A screenshot run's throwaway root thus reads like a fresh install's.
    std::string displayPath(const std::string& path) const;

    // Where a fresh by-name save goes: "<instanceDir>/<name>.card.yaml" /
    // ".floppy.yaml" (namedFileName()'s rules strip separators etc.).
    std::string instancePathFor(const std::string& instanceName) const;
    std::string floppyInstancePathFor(const std::string& diskName) const;

    // Cassette tapes: Settings::tapeDirOverride() if set, else "Tapes"
    // inside instanceDir(); created on demand. A tape named `tapeName` is
    // "<tapeName>.wav" there.
    std::string tapeDir() const;
    std::string tapePathFor(const std::string& tapeName) const;

    // A save of a card or a floppy under a name (Name & Save, a preset's
    // `saveas:`) by the shared rules (Connector/MediaSave.hpp): the path in
    // *path, and the refusal (None if allowed).
    MediaSaveRefusal planNamedSave(MediaKind kind, const std::string& name, const std::string& explicitPath,
                                   bool asTemplate, std::string* path) const;

private:
    const Settings& m_settings;
    HostDirs m_dirs;
    std::string m_isolatedInstanceDir;
};

// "a/b" joined the way every path here is built (forward slashes; an
// absolute `name` wins).
std::string joinPath(const std::string& dir, const std::string& name);

// True if `path` resolves to somewhere inside `dir` (both canonicalized;
// false if either doesn't exist). Whole path components only:
// "<dir>-backup/x" is not under "<dir>".
bool isUnderDir(const std::string& path, const std::string& dir);

} // namespace shared
