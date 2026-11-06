#include "TapeManager.hpp"


TapeManager::TapeManager(MachineController* controller, QObject* parent)
    : QObject(parent), m_bay(&controller->app().tapes()) {
    connect(controller, &MachineController::tapeListChanged, this, &TapeManager::tapesChanged);
    connect(controller, &MachineController::bayErrorMessage, this, [this](shared::Bay bay, const QString& text) {
        if (bay == shared::Bay::Tape) emit errorMessage(text);
    });
}

QStringList TapeManager::pickerNames() const {
    QStringList names;
    for (const std::string& n : m_bay->pickerNames()) names << QString::fromStdString(n);
    return names;
}

bool TapeManager::selectForPlay(const QString& nameOrEmpty, QString* error) {
    return withQtError(error, [&](std::string* err) { return m_bay->selectForPlay(nameOrEmpty.toStdString(), err); });
}

bool TapeManager::recordNew(const QString& name, QString* error) {
    return withQtError(error, [&](std::string* err) { return m_bay->recordNew(name.toStdString(), err); });
}
