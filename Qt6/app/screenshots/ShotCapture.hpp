#pragma once
#include <QImage>
#include <QList>
#include <QRect>
#include <QString>

class QWidget;

// Image capture for screenshot scenarios (ShotRunner).
namespace ShotCapture {

// Every visible transient top-level that should appear in a shot of
// `main`: combo-box drop-downs, Qt menus, dialogs (not `main` itself, no
// tooltips).
QList<QWidget*> visibleTransients(const QWidget* main);

// `method: qt`: renders `target` plus `extras` (each at its on-screen
// position) into one image, `padding` logical px around their union, at
// `scale` device px per logical px. Rendered via QWidget::render(), so the
// result is independent of the screen's own pixel ratio and of whatever
// overlaps the window on screen. Transparent background outside the
// widgets.
QImage renderComposite(QWidget* target, const QList<QWidget*>& extras, int padding, double scale);

// `method: system` (macOS): `screencapture` of the window `widget` belongs
// to, title bar included (`-l`, no shadow).
bool systemCaptureWindow(const QWidget* widget, const QString& file, QString* error);
// `method: system` (macOS): `screencapture` of a global screen rect -- the
// route for native menu-bar menus, which no Qt API can see.
bool systemCaptureRect(const QRect& globalRect, const QString& file, QString* error);

bool savePng(const QImage& image, const QString& file, QString* error);

} // namespace ShotCapture
