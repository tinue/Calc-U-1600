#pragma once
#include <QObject>
#include <QString>
#include <QVector>

#include "MachineController.hpp"
#include "Connector/MemoryCardDefinition.hpp"  // CardHost
#include "Shared/ModuleBay.hpp"

// The control bar's memory-module slots: the session's shared::ModuleBay
// (Shared/ModuleBay.hpp has the rules -- selection, rebuild-time attach,
// battery autosave, Name & Save, preset save-as) with Qt signals and
// QStrings. Its autosave runs in AppSession::frameDuties().
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

    QString selectedModuleName(int slot) const { return QString::fromStdString(m_bay->selectedModuleName(slot)); }
    bool canNameAndSave(int slot) const { return m_bay->canNameAndSave(slot); }
    bool nameAndSave(int slot, const QString& instanceName, QString* error);

    void flushPendingPersist() { m_bay->flushPendingPersist(); }

signals:
    void moduleChanged(int slot);
    void errorMessage(const QString& text);

private:
    shared::ModuleBay* m_bay;  // the session's
};
