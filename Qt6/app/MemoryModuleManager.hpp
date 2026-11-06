#pragma once
#include <QObject>
#include <QString>
#include <QVector>
#include <memory>

#include "MachineController.hpp"
#include "Connector/MemoryCardDefinition.hpp"  // CardHost
#include "Shared/ModuleBay.hpp"

// The control bar's memory-module slots: shared::ModuleBay
// (Shared/ModuleBay.hpp has the rules -- selection, rebuild-time attach,
// battery autosave, Name & Save, preset save-as) with Qt signals and
// QStrings.
class MemoryModuleManager : public QObject {
    Q_OBJECT
public:
    explicit MemoryModuleManager(MachineController* controller, QObject* parent = nullptr);

    struct ModuleEntry {
        QString moduleName;
        bool battery = false;
        bool rom = false;
    };
    struct ModuleLists {
        QVector<ModuleEntry> templates;
        QVector<ModuleEntry> instances;
    };
    ModuleLists moduleLists(CardHost host) const;
    static CardHost hostForModel(int slot, Model model) { return shared::ModuleBay::hostForModel(slot, model); }

    void selectModule(int slot, const QString& moduleNameOrEmpty) { m_bay->selectModule(slot, moduleNameOrEmpty.toStdString()); }
    QString selectedModuleName(int slot) const { return QString::fromStdString(m_bay->selectedModuleName(slot)); }
    bool slotHasInstanceFile(int slot) const { return m_bay->slotHasInstanceFile(slot); }
    bool canNameAndSave(int slot) const { return m_bay->canNameAndSave(slot); }
    void attachAllToFreshMachine() { m_bay->attachAllToFreshMachine(); }
    void syncFromPresetLoad(int slot, const QString& resolvedPathOrEmpty = QString(), const QString& romFile = QString()) {
        m_bay->syncFromPresetLoad(slot, resolvedPathOrEmpty.toStdString(), romFile.toStdString());
    }
    bool nameAndSave(int slot, const QString& instanceName, QString* error);
    bool saveAsFromPreset(int slot, const QString& instanceName, const QString& filePath, bool asTemplate,
                          QString* error);

    // Once per frame tick: marks the live cards dirty and writes what the
    // debounce lets through.
    void markDirtyAndSchedulePersist();
    void flushPendingPersist() { m_bay->flushPendingPersist(); }
    void onModelChanged() { m_bay->onModelChanged(); }

signals:
    void moduleChanged(int slot);
    void errorMessage(const QString& text);

private:
    std::unique_ptr<shared::ModuleBay> m_bay;
};
