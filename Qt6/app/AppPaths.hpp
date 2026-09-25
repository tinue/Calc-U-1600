#pragma once
#include <QString>
#include <string>

// Path resolution for bundled resources and the memory-module feature --
// the one place that knows where bundled resources (*.card.yaml templates
// AND ROM images, side by side) and user-writable battery-card instances
// live on each platform. Bundled resources ship as loose files next to the
// executable (not Qt's .qrc embedding, since Core's
// scanMemoryCardDirectory()/BundledRomCatalog read a real filesystem
// directory), and the writable instance directory defaults to
// ~/Calc-U-1600 on every platform (not QStandardPaths::AppDataLocation, no
// iCloud).
namespace AppPaths {

// Where the bundled resources live -- *.card.yaml catalog AND ROM images
// (disjoint filename extensions, one shared directory) -- resolved
// relative to the running executable: macOS .app bundle's
// Contents/Resources (via applicationDirPath()'s Contents/MacOS +
// "../Resources"), or a "resources" folder next to the binary on
// Windows/Linux (matching windeployqt's and AppImage's flat-next-to-binary
// convention). Read-only bundled content -- never created if missing; a
// missing/unreadable directory is tolerated by scanMemoryCardDirectory()/
// BundledRomCatalog (empty catalogue / resolve failure).
QString bundledResourcesDir();

// ~/Calc-U-1600 (home dir, non-dotted), created if missing.
QString defaultInstanceDir();

// Screenshot runs (`--shots`, main.cpp) point instanceDir() at a throwaway
// folder, so the user's saved cards/disks stay out of the pictures and
// untouched. Not a setting: Settings keeps showing the default, see
// forDisplay().
void setIsolatedInstanceDir(const QString& dir);

// `path` as the Settings dialog shows it: inside the isolated folder of a
// screenshot run, mapped back onto defaultInstanceDir(), so the images show
// what a fresh install shows. Otherwise unchanged.
QString forDisplay(const QString& path);

// AppSettings::instanceDirOverride() if non-empty, else the isolated
// folder of a screenshot run if set, else defaultInstanceDir(). The single source of truth every other class
// calls to find "where battery-card instances live right now".
QString instanceDir();

// Strips path separators so a user-typed instance name can't escape the
// directory, collapses whitespace, "Untitled" fallback, ".card.yaml" suffix.
QString sanitizedInstanceFileName(const QString& instanceName);

// The full path a fresh save under `instanceName` should write to.
QString instancePathFor(const QString& instanceName);

// Same as sanitizedInstanceFileName()/instancePathFor(), but for CE-1600F
// floppy disks: "<name>.floppy.yaml" (Connector/FloppyImageFile.hpp)
// instead of "<name>.card.yaml".
QString sanitizedFloppyFileName(const QString& diskName);
QString floppyInstancePathFor(const QString& diskName);

// True if `path` resolves to somewhere inside `dir` (both canonicalized;
// false if either doesn't exist).
bool isUnderDir(const QString& path, const QString& dir);

// Atomic write via QSaveFile (writes to a temp file beside `path` and
// renames on commit).
bool atomicWriteFile(const QString& path, const std::string& text);

}  // namespace AppPaths
