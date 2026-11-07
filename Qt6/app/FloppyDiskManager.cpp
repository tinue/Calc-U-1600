#include "FloppyDiskManager.hpp"

namespace {

QVector<FloppyDiskManager::DiskEntry> toQt(const shared::FloppyBay& bay, const std::vector<std::string>& names) {
    QVector<FloppyDiskManager::DiskEntry> out;
    for (const std::string& n : names)
        out.push_back({QString::fromStdString(n), QString::fromStdString(bay.diskDescription(n))});
    return out;
}

} // namespace

FloppyDiskManager::FloppyDiskManager(MachineController* controller, QObject* parent)
    : QObject(parent), m_bay(&controller->app().floppy()) {
    connect(controller, &MachineController::bayErrorMessage, this, [this](shared::Bay bay, const QString& text) {
        if (bay == shared::Bay::Floppy) emit errorMessage(text);
    });
}

FloppyDiskManager::DiskLists FloppyDiskManager::diskLists() const {
    const shared::FloppyBay::DiskLists lists = m_bay->diskLists();
    return {toQt(*m_bay, lists.templates), toQt(*m_bay, lists.instances)};
}

bool FloppyDiskManager::nameAndSave(const QString& diskName, QString* error) {
    return withQtError(error, [&](std::string* err) { return m_bay->nameAndSave(diskName.toStdString(), err); });
}
