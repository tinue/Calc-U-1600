#pragma once

// Redirects Qt's qDebug()/qWarning()/qCritical()/qFatal() output to a
// platform-appropriate destination. Windows builds link the GUI
// subsystem (WIN32 in CMakeLists.txt's qt_add_executable() call) so
// there's no console for those messages to reach;
// Linux routes them to syslog so they land wherever the distro's log
// tooling (journald, /var/log/syslog, ...) already looks. macOS is left
// alone -- the default Qt handler writes to stderr, which Console.app
// already captures automatically for a GUI-launched process.
namespace AppLogging {

// Call once, before constructing QApplication.
void install();

}  // namespace AppLogging
