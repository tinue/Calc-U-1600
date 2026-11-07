#include "MemoryModuleManager.hpp"

namespace {

QVector<MemoryModuleManager::ModuleEntry> toQt(const std::vector<shared::ModuleBay::ModuleEntry>& entries) {
    QVector<MemoryModuleManager::ModuleEntry> out;
    for (const auto& e : entries) out.push_back({QString::fromStdString(e.moduleName), QString::fromStdString(e.description), e.battery, e.rom});
    return out;
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
    return {toQt(lists.templates), toQt(lists.instances), toQt(lists.roms)};
}

bool MemoryModuleManager::nameAndSave(int slot, const QString& instanceName, QString* error) {
    return withQtError(error, [&](std::string* err) { return m_bay->nameAndSave(slot, instanceName.toStdString(), err); });
}
