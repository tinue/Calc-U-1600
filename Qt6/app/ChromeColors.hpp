#pragma once
#include <QColor>
#include <QPalette>
#include <QWidget>

// Shared light/dark chrome palette for the docked panels beneath the
// calculator body (DebugPanel, PlotterPaperWidget) -- factored out so "same
// backdrop color as the debug panel" is literally the same constant, not an
// independently eyeballed copy that can drift.
struct ChromeColors {
    QColor headerBackground;
    QColor headerText;
    QColor outputBackground;
    QColor outputText;
    QColor outputBorder;
    QColor buttonBarBackground;
    QColor pillBackground;    // "on"/active-looking
    QColor pillBackgroundOff; // dimmed/unavailable
    QColor pillText;

    static bool isDarkMode(const QWidget* w) {
        return w->palette().color(QPalette::Window).lightness() < 128;
    }

    static ChromeColors forWidget(const QWidget* w) { return isDarkMode(w) ? dark() : light(); }

    static ChromeColors light() {
        return ChromeColors{
            QColor(204, 204, 204), QColor(0, 0, 0, 140),
            QColor(0xF7, 0xF7, 0xF7), QColor(0, 0, 0, 204), QColor(153, 153, 153),
            QColor(217, 217, 217),
            QColor(173, 173, 173), QColor(199, 199, 199), QColor(0, 0, 0, 204),
        };
    }

    static ChromeColors dark() {
        return ChromeColors{
            QColor(60, 60, 60), QColor(255, 255, 255, 150),
            QColor(32, 32, 32), QColor(255, 255, 255, 210), QColor(90, 90, 90),
            QColor(48, 48, 48),
            QColor(120, 120, 120), QColor(66, 66, 66), QColor(255, 255, 255, 220),
        };
    }
};

inline QString cssRgba(const QColor& c) {
    return QString("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

// Shared stylesheet fragments built from ChromeColors, so DebugPanel and
// PlotterPaperWidget's docked-panel chrome (header bar, button bar, pill-
// shaped buttons) can't independently drift from the same-looking backdrop.
namespace ChromeStyle {

inline QString header(const ChromeColors& c) {
    return QString("background-color: %1; color: %2; padding: 6px 0;")
        .arg(cssRgba(c.headerBackground), cssRgba(c.headerText));
}

inline QString buttonBar(const ChromeColors& c) {
    return QString("background-color: %1;").arg(cssRgba(c.buttonBarBackground));
}

// The pill look's shared core declarations (radius/padding/colors) --
// callers wrap this in their own selector (QToolButton/QPushButton) and add
// whatever :hover/:pressed/:disabled rules that selector needs.
inline QString pillCore(const QColor& background, const QColor& text) {
    return QString("border-radius: 6px; padding: 4px 8px; background-color: %1; color: %2;")
        .arg(cssRgba(background), cssRgba(text));
}

// A pill-styled QPushButton with a visible pressed state (the pane action
// buttons: plotter paper Copy/Cut, CE-158 printer Save/Clear).
inline QString pillPushButton(const ChromeColors& c) {
    return QString("QPushButton { border: none; %1 }"
                   "QPushButton:hover:!disabled { background-color: %2; }"
                   "QPushButton:pressed, QPushButton:hover:pressed { background-color: %3; }"
                   "QPushButton:disabled { background-color: %4; color: %5; }")
        .arg(pillCore(c.pillBackground, c.pillText), cssRgba(c.pillBackground.lighter(115)),
             cssRgba(c.pillBackground.darker(130)), cssRgba(c.pillBackgroundOff), cssRgba(c.pillText));
}

} // namespace ChromeStyle
