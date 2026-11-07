#pragma once
#include <QString>
#include <string>

#include "Shared/AppPaths.hpp"

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

// The shared::AppPaths every call below forwards to (Shared/AppPaths.hpp
// has the rules); built on first use from the bundled resources folder
// next to the executable and the home folder.
shared::AppPaths& paths();

// ~/Calc-U-1600 (home dir, non-dotted) -- or, in a screenshot run, the
// throwaway folder set by setIsolatedInstanceDir() -- created if missing.
QString defaultInstanceDir();

// Screenshot runs (`--shots`, main.cpp) make a throwaway folder the
// default storage root, so the user's saved cards/disks stay out of the
// pictures and untouched. Not a setting.
void setIsolatedInstanceDir(const QString& dir);

// `path` the way every label shows one: under defaultInstanceDir() as
// "~/Calc-U-1600/...", else under the home folder as "~/...", else as is.
// A screenshot run's throwaway root thus reads like a fresh install's.
QString displayPath(const QString& path);

// AppSettings::instanceDirOverride() if non-empty, else
// defaultInstanceDir(). The single source of truth every other class calls
// to find "where battery-card instances live right now".
QString instanceDir();

// Cassette tapes (TapeManager): AppSettings::tapeDirOverride() if
// non-empty, else "Tapes" inside instanceDir(); created on demand. A tape
// named `tapeName` is "<tapeName>.wav" there (namedFileName()'s rules).
QString tapeDir();

// shared::AppPaths::traceDir() / serialLinkDir(): the Settings override,
// else instanceDir().
QString traceDir();
QString serialLinkDir();

}  // namespace AppPaths
