#include "MemoryModuleManager.hpp"


namespace {

QVector<MemoryModuleManager::ModuleEntry> toQt(const std::vector<shared::ModuleBay::ModuleEntry>& entries) {
    QVector<MemoryModuleManager::ModuleEntry> out;
    for (const auto& e : entries) out.push_back({QString::fromStdString(e.moduleName), e.battery, e.rom});
    return out;
}

bool reportResult(bool ok, const std::string& err, QString* error) {
    if (!ok && error) *error = QString::fromStdString(err);
    return ok;
}


} // namespace

MemoryModuleManager::MemoryModuleManager(MachineController* controller, QObject* parent)
    : QObject(parent), m_bay(&controller->app().modules()) {
    connect(controller, &MachineController::bayModuleChanged, this, &MemoryModuleManager::moduleChanged);
    connect(controller, &MachineController::bayErrorMessage, this, [this](shared::Bay bay, const QString& text) {
        if (bay == shared::Bay::Modules) emit errorMessage(text);
    });
}

MemoryModuleManager::ModuleLists MemoryModuleManager::moduleLists(CardHost host) const {
    const shared::ModuleBay::ModuleLists lists = m_bay->moduleLists(host);
    return {toQt(lists.templates), toQt(lists.instances)};
}

bool MemoryModuleManager::nameAndSave(int slot, const QString& instanceName, QString* error) {
    std::string err;
    return reportResult(m_bay->nameAndSave(slot, instanceName.toStdString(), &err), err, error);
}

bool MemoryModuleManager::saveAsFromPreset(int slot, const QString& instanceName, const QString& filePath,
                                           bool asTemplate, QString* error) {
    std::string err;
    return reportResult(
        m_bay->saveAsFromPreset(slot, instanceName.toStdString(), filePath.toStdString(), asTemplate, &err), err,
        error);
}

