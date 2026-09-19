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
// Persistence is a versioned `<name>.floppy.yaml` (Connector/
// FloppyImageFile.hpp), found by its `disk-name` -- bundled directory
// first, then the user's save folder, the same order memory cards use. It
// is always rewritten whole (no battery-card-style splice: a disk image
// carries no hand-written prose worth preserving).
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

    // Side A/B -- the software analogue of ejecting and flipping the
    // physical disk (CE1600FCard::setSide()'s own comment: this itself
    // re-arms the drive's changed-disk latch, matching the user manual's
    // "when the green lamp goes off, eject the disk, turn it over"
    // instruction). 0 = A, 1 = B; toggleSide() flips between them.
    int side() const;
    void toggleSide();
    // The "green lamp" -- true while the drive motor is spinning, so the
    // control bar can show the user when it's safe to flip the disk.
    bool motorOn() const;

    // Loads the currently selected disk (or the empty default, if none is
    // selected) into a just-attached CE1600FCard. PlotterController calls
    // this as part of its CE-1600P attach, before announcing it -- the GUI
    // counterpart of the preset loader loading its `floppy:` at attach.
    void insertSelectedDisk();

    // Mirrors MemoryModuleManager::syncFromPresetLoad() -- called by
    // PresetController after a preset's `floppy:` key resolved and loaded
    // a disk directly on the Core machine, so this manager's bookkeeping
    // matches without re-attaching anything.
    void syncFromPresetLoad(const QString& labelOrEmpty, const QString& resolvedPathOrEmpty = QString());

    // "Name & Save" flow, mirroring MemoryModuleManager's. A bundled name
    // is always refused (the bundled disk would shadow the saved one).
    bool isBundledName(const QString& diskName) const;
    bool nameCollides(const QString& diskName) const;
    bool nameAndSave(const QString& diskName, QString* error);

    void markDirtyAndSchedulePersist();  // called once per frame tick
    void flushPendingPersist();          // called before select/model-switch/quit

signals:
    void errorMessage(const QString& text);

private:
    MachineController* m_controller;  // not owned

    QString m_diskName;         // empty = blank disk
    QString m_instanceFilePath; // empty unless resolved from the instance dir
    bool m_persistPending = false;
    uint64_t m_lastSeenRevision = 0;

    // Single-shot, 500ms. Not restarted by further writes, so a long disk
    // operation still autosaves every 500ms (same as MemoryModuleManager).
    QTimer* m_debounceTimer = nullptr;

    // Resolves and loads m_diskName into the card; false (after reporting
    // any error) if it's empty or couldn't be loaded.
    bool loadSelectedDisk(PC1600Machine* m1600);
    void writeInstance();  // rewrite m_instanceFilePath from the live image
};
