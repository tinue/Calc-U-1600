#include "MemoryModuleManager.hpp"

#include <chrono>

#include "AppPaths.hpp"

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

double monotonicSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

MemoryModuleManager::MemoryModuleManager(MachineController* controller, QObject* parent) : QObject(parent) {
    shared::ModuleBay::Events events;
    events.moduleChanged = [this](int slot) { emit moduleChanged(slot); };
    events.error = [this](const std::string& text) { emit errorMessage(QString::fromStdString(text)); };
    m_bay = std::make_unique<shared::ModuleBay>(controller->session(), AppPaths::paths(), std::move(events));
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

void MemoryModuleManager::markDirtyAndSchedulePersist() {
    m_bay->markDirty();
    m_bay->tick(monotonicSeconds());
}
