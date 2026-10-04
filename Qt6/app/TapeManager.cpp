#include "TapeManager.hpp"

#include <QDir>
#include <QFileInfo>

#include "AppPaths.hpp"

TapeManager::TapeManager(MachineController* controller, QObject* parent)
    : QObject(parent), m_controller(controller) {}

QStringList TapeManager::tapeNames() const {
    QStringList names;
    const QFileInfoList files =
        QDir(AppPaths::tapeDir()).entryInfoList({QStringLiteral("*.wav")}, QDir::Files, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& f : files) names << f.completeBaseName();
    return names;
}

QString TapeManager::currentName() const {
    const TapeDeck::Status status = m_controller->tapeStatus();
    if (status.mode == TapeDeck::Mode::Empty) return {};
    return QFileInfo(QString::fromStdString(status.path)).completeBaseName();
}

bool TapeManager::exists(const QString& name) const {
    return QFileInfo::exists(AppPaths::tapePathFor(name));
}

bool TapeManager::selectForPlay(const QString& nameOrEmpty, QString* error) {
    m_controller->tapeEject(error);
    if (nameOrEmpty.isEmpty()) return true;
    return m_controller->tapePlay(AppPaths::tapePathFor(nameOrEmpty), error);
}

bool TapeManager::recordNew(const QString& name, QString* error) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        *error = tr("Name cannot be empty.");
        return false;
    }
    m_controller->tapeRecord(AppPaths::tapePathFor(trimmed)); // ejects what was in
    return true;
}

void TapeManager::onFrameTick(const TapeDeck::Status& status) {
    const bool recordingMotor = status.mode == TapeDeck::Mode::Record && status.motor;
    if (m_recordingMotor && !recordingMotor) emit tapesChanged(); // saved at the motor stop
    m_recordingMotor = recordingMotor;
    const QString error = m_controller->tapeTakeLastError();
    if (!error.isEmpty()) emit errorMessage(error);
}
