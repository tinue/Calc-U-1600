#include "TapeManager.hpp"

#include "AppPaths.hpp"

TapeManager::TapeManager(MachineController* controller, QObject* parent) : QObject(parent) {
    shared::TapeBay::Events events;
    events.tapesChanged = [this] { emit tapesChanged(); };
    events.error = [this](const std::string& text) { emit errorMessage(QString::fromStdString(text)); };
    m_bay = std::make_unique<shared::TapeBay>(controller->session(), AppPaths::paths(), std::move(events));
}

QStringList TapeManager::tapeNames() const {
    QStringList names;
    for (const std::string& n : m_bay->tapeNames()) names << QString::fromStdString(n);
    return names;
}

bool TapeManager::selectForPlay(const QString& nameOrEmpty, QString* error) {
    std::string err;
    const bool ok = m_bay->selectForPlay(nameOrEmpty.toStdString(), &err);
    if (!ok && error) *error = QString::fromStdString(err);
    return ok;
}

bool TapeManager::recordNew(const QString& name, QString* error) {
    if (m_bay->recordNew(name.toStdString())) return true;
    *error = tr("Name cannot be empty.");
    return false;
}
