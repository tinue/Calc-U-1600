#pragma once
#include <QRect>

class QWidget;

#ifdef Q_OS_MACOS
// Cocoa bits the `method: system` screenshot path needs (ShotCapture).

// The window-server number of `widget`'s top-level window -- what
// `screencapture -l` takes. 0 if it has none (not shown yet).
long macWindowNumber(const QWidget* widget);

// Union, in global screen points (Qt's coordinate space), of every on-screen
// window this process owns: the main window, dialogs, and -- unlike Qt's own
// top-level list -- native NSMenu drop-downs. `menusOnly`: just the windows
// above the normal level (open menus). Empty if there are none.
QRect macOwnWindowsBounds(bool menusOnly = false);

// Brings the app to the front, so its menus own the menu bar.
void macActivateApp();
#endif
