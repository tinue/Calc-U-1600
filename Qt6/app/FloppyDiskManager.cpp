#include "FloppyDiskManager.hpp"

namespace {

QVector<FloppyDiskManager::DiskEntry> toQt(const std::vector<shared::FloppyBay::DiskEntry>& entries) {
    QVector<FloppyDiskManager::DiskEntry> out;
    for (const auto& e : entries)
        out.push_back({QString::fromStdString(e.diskName), QString::fromStdString(e.description)});
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
    return {toQt(lists.templates), toQt(lists.instances)};
}

bool FloppyDiskManager::nameAndSave(const QString& diskName, QString* error) {
    return withQtError(error, [&](std::string* err) { return m_bay->nameAndSave(diskName.toStdString(), err); });
}
