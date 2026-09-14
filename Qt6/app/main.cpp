#include <QApplication>
#include <QIcon>
#include "AppLogging.hpp"
#include "MainWindow.hpp"

int main(int argc, char** argv) {
    AppLogging::install();
    QApplication app(argc, argv);
    // Without this, the running window's title-bar/taskbar icon is
    // whatever the platform defaults to (e.g. a generic AppImage icon on
    // Linux) -- the .desktop file's Icon= only covers desktop-environment
    // integration (launcher/file-manager icon), not the live window.
    app.setWindowIcon(QIcon(":/app/icon.png"));
    MainWindow window;
    window.show();
    return app.exec();
}
