#include "MachineController.hpp"

#include <cstdlib>

#include <QDebug>
#include <QMessageBox>
#include <QtGlobal>

#include "AppPaths.hpp"
#include "AppSettings.hpp"
#include "debug/DebugController.hpp"

MachineController::MachineController(QObject* parent) : QObject(parent) {
    shared::AppSession::Platform& platform = *this;
    m_app = std::make_unique<shared::AppSession>(AppSettings::settings(), AppPaths::paths(), platform);
    m_debug = std::make_unique<DebugController>(this);
    m_app->start();
    m_debug->refreshServer();
}

MachineController::~MachineController() {
    // The debugger goes first: its target refers to the live machine.
    m_debug.reset();
    m_app.reset();
}

// A missing/unreadable bundled ROM means the install is broken -- there's
// no usable machine to fall back to (this is reached both at startup and
// from a later model switch, and either way the app can't proceed without
// its firmware). Show the user what's wrong and where Calc-U-1600 looked,
// then exit: an abort (qFatal) would be a silent crash for an app not
// launched from a terminal. std::exit() (not returning to unwind the call
// stack) is deliberate: this can be reached mid-construction of
// MainWindow/MachineController, which aren't set up to unwind safely from
// here, and terminating the process reclaims everything the OS owns
// regardless.
void MachineController::romMissing(const std::string& error, const std::string& searchedIn) {
    const QString message =
        QObject::tr("Calc-U-1600 could not start:\n\n%1\n\nSearched in: %2\n\n"
                     "If you built this from source, run tools/fetch_roms.sh to "
                     "download the required ROM files. If you're running an "
                     "installed copy, the app's ROM files may be missing or "
                     "corrupted -- try reinstalling.")
            .arg(QString::fromStdString(error), QString::fromStdString(searchedIn));
    QMessageBox::critical(nullptr, QObject::tr("Missing ROM File"), message);
    std::exit(1);
}

void MachineController::notice(shared::MachineSession::Notice notice, const std::string& detail) {
    const QString d = QString::fromStdString(detail);
    switch (notice) {
        case shared::MachineSession::Notice::OldPC1600RomUnavailable:
            QMessageBox::warning(nullptr, QObject::tr("Old ROM unavailable"),
                                 QObject::tr("The old PC-1600 ROM could not be loaded:\n\n%1\n\n"
                                             "Using the new ROM instead.")
                                     .arg(d));
            break;
        case shared::MachineSession::Notice::OldCE1600PRomUnavailable:
            QMessageBox::warning(nullptr, QObject::tr("Old CE-1600P ROM unavailable"),
                                 QObject::tr("The old CE-1600P ROM could not be loaded:\n\n%1\n\n"
                                             "Using the new ROM instead.")
                                     .arg(d));
            break;
        case shared::MachineSession::Notice::TapeSaveFailed:
            qWarning("Tape: %s", qPrintable(d));
            break;
    }
}

void MachineController::machineAboutToChange() {
    if (m_debug) m_debug->machineAboutToChange();
}

void MachineController::machineReplaced() {
    if (m_debug) m_debug->machineReplaced();
}

void MachineController::bayError(shared::Bay bay, const std::string& text) {
    emit bayErrorMessage(bay, QString::fromStdString(text));
}

void MachineController::presetLog(const std::string& line) {
    qDebug().noquote() << "[preset]" << QString::fromStdString(line);
}

void MachineController::attachFailed(const std::string& device, const std::string& reason) {
    emit peripheralAttachFailed(QString::fromStdString(device), QString::fromStdString(reason));
}

bool MachineController::runDebuggerSlice(std::uint64_t cycles) {
    if (!m_debug || !m_debug->attached()) return false;
    m_debug->runSlice(cycles);
    return true;
}

void MachineController::refreshDebugServer() { m_debug->refreshServer(); }

bool MachineController::tapeEject(QString* error) {
    return withQtError(error, [this](std::string* err) { return session().tapeEject(err); });
}
