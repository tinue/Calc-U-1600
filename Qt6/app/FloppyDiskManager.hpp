#pragma once
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <cstdint>

#include "MachineController.hpp"

// Owns the CE-1600F floppy-disk selection/dirty/autosave state for the
// control bar's disk picker -- the floppy counterpart to
// MemoryModuleManager, but simpler: there is no CardHost/slot concept
// (the floppy isn't CPU-address-mapped; see CE1600FCard.hpp), just one
// disk image, and attach/detach is entirely PlotterController's job
// (CE-1600F/P attach as a union -- PC1600Machine::attachCE1600P()) --
// this class only owns which *disk image* is loaded once the floppy is
// attached.
//
// Persistence is a raw `<name>.floppy.img` (64KB, byte-for-byte) plus a
// small `<name>.floppy.yaml` metadata sidecar -- not the battery-card
// splice-into-YAML-text mechanism (BatteryCardInstance.hpp): a disk image
// carries no hand-written prose worth preserving that way, and hex-
// dumping 64KB as YAML text would run about 4x the byte count for no
// benefit. See AppPaths::sanitizedFloppyFileName()/floppyInstancePathFor().
class FloppyDiskManager : public QObject {
    Q_OBJECT
public:
    explicit FloppyDiskManager(MachineController* controller, QObject* parent = nullptr);

    struct DiskEntry {
        QString diskName;
    };

    // Catalogue queries for ControlBar's disk combo-box population.
    QVector<DiskEntry> bundledEntries() const;
    QVector<DiskEntry> instanceEntries() const;

    // Empty diskNameOrEmpty => blank disk. A live hot-swap (no power-cycle
    // needed -- unlike memory-card slots, the floppy isn't sized/scanned
    // by the boot ROM at reset; a real drive lets you swap diskettes while
    // powered). No-op if no floppy is currently attached.
    void selectDisk(const QString& diskNameOrEmpty);
    QString selectedDiskName() const { return m_diskName; }
    bool hasInstanceFile() const { return !m_instanceFilePath.isEmpty(); }

    // Called whenever CE-1600P (and therefore the floppy, per the union
    // attach) transitions to attached -- pushes the currently selected
    // disk image (or leaves the freshly-inserted blank default, if none
    // is selected) into the just-attached CE1600FCard. PlotterController
    // itself only knows ROM bytes, not disk images, so MainWindow calls
    // this right after PlotterController::ce1600pAttachedChanged(true).
    void attachToMachine();

    // Mirrors MemoryModuleManager::syncFromPresetLoad() -- called by
    // PresetController after a preset's `floppy:` key resolved and loaded
    // a disk directly on the Core machine, so this manager's bookkeeping
    // matches without re-attaching anything.
    void syncFromPresetLoad(const QString& labelOrEmpty, const QString& resolvedPathOrEmpty = QString());

    // "Name & Save" flow, mirroring MemoryModuleManager's.
    bool nameCollides(const QString& diskName) const;
    bool nameAndSave(const QString& diskName, QString* error);

    void markDirtyAndSchedulePersist();  // called once per frame tick
    void flushPendingPersist();          // called before select/model-switch/quit

signals:
    void diskChanged();
    void errorMessage(const QString& text);

private:
    MachineController* m_controller;  // not owned

    QString m_diskName;         // empty = blank disk
    QString m_instanceFilePath; // empty unless resolved from the instance dir
    bool m_persistPending = false;
    uint64_t m_lastSeenRevision = 0;

    QTimer* m_debounceTimer = nullptr;  // single-shot, 500ms, restarted while dirty

    void writeInstance();  // rewrite m_instanceFilePath from the live image
};
