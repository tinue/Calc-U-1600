#include "FloppyDiskManager.hpp"


namespace {

QVector<FloppyDiskManager::DiskEntry> toQt(const std::vector<std::string>& names) {
    QVector<FloppyDiskManager::DiskEntry> out;
    for (const std::string& n : names) out.push_back({QString::fromStdString(n)});
    return out;
}

bool reportResult(bool ok, const std::string& err, QString* error) {
    if (!ok && error) *error = QString::fromStdString(err);
    return ok;
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
    std::string err;
    return reportResult(m_bay->nameAndSave(diskName.toStdString(), &err), err, error);
}

bool FloppyDiskManager::saveAsFromPreset(const QString& diskName, const QString& filePath, bool asTemplate,
                                         QString* error) {
    std::string err;
    return reportResult(m_bay->saveAsFromPreset(diskName.toStdString(), filePath.toStdString(), asTemplate, &err),
                        err, error);
}

