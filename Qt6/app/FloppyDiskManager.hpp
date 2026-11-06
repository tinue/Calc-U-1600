#pragma once
#include <QObject>
#include <QString>
#include <QVector>
#include <memory>

#include "MachineController.hpp"
#include "Shared/FloppyBay.hpp"

// The control bar's CE-1600F disk picker: shared::FloppyBay
// (Shared/FloppyBay.hpp has the rules -- selection, side, autosave, Name &
// Save, preset save-as) with Qt signals and QStrings.
class FloppyDiskManager : public QObject {
    Q_OBJECT
public:
    explicit FloppyDiskManager(MachineController* controller, QObject* parent = nullptr);

    struct DiskEntry {
        QString diskName;
    };
    struct DiskLists {
        QVector<DiskEntry> templates;
        QVector<DiskEntry> instances;
    };
    DiskLists diskLists() const;

    void selectDisk(const QString& diskNameOrEmpty) { m_bay->selectDisk(diskNameOrEmpty.toStdString()); }
    QString selectedDiskName() const { return QString::fromStdString(m_bay->selectedDiskName()); }
    bool canNameAndSave() const { return m_bay->canNameAndSave(); }
    int side() const { return m_bay->side(); }
    void toggleSide() { m_bay->toggleSide(); }
    bool motorOn() const { return m_bay->motorOn(); }
    void insertSelectedDisk() { m_bay->insertSelectedDisk(); }
    void syncFromPresetLoad(const QString& labelOrEmpty, const QString& resolvedPathOrEmpty = QString()) {
        m_bay->syncFromPresetLoad(labelOrEmpty.toStdString(), resolvedPathOrEmpty.toStdString());
    }
    bool nameAndSave(const QString& diskName, QString* error);
    bool saveAsFromPreset(const QString& diskName, const QString& filePath, bool asTemplate, QString* error);

    // Once per frame tick: notices a changed disk and writes what the
    // debounce lets through.
    void markDirtyAndSchedulePersist();
    void flushPendingPersist() { m_bay->flushPendingPersist(); }

signals:
    void errorMessage(const QString& text);

private:
    std::unique_ptr<shared::FloppyBay> m_bay;
};
