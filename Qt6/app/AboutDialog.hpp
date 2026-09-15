#pragma once
#include <QDialog>

// Minimal About dialog: app name, version + build id (see Version.h.in),
// and a pointer to the license files already shipped alongside the app --
// same single-"Close"-button shape as SettingsDialog.
class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent = nullptr);
};
