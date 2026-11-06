#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class PC1600Machine;

namespace shared {

class AppPaths;
class MachineSession;

// ── FloppyBay: the CE-1600F disk picker ──────────────────────────────────
//
// Which disk image is in the CE-1600F drive, its side, and its autosave --
// the floppy counterpart to ModuleBay, but simpler: no slots (the floppy
// isn't CPU-address-mapped), just one disk image, and the drive comes and
// goes with the CE-1600P (attach/detach is the plotter's business; this
// only owns which disk is loaded once the drive is there).
//
// Persistence is a versioned `<name>.floppy.yaml` (Connector/
// FloppyImageFile.hpp), found by its `disk-name` -- bundled directory
// first, then the save folder, as for memory cards. It is always rewritten
// whole. An instance disk autosaves at most every kPersistDebounceSeconds
// after the drive wrote to it (tick() each frame).
class FloppyBay {
public:
    struct Events {
        std::function<void(const std::string&)> error;
    };
    struct DiskLists {
        std::vector<std::string> templates;  // `template: true`: every bundled disk + user templates
        std::vector<std::string> instances;  // the save folder's other disks (bundled names left out)
    };

    static constexpr double kPersistDebounceSeconds = 0.5;

    FloppyBay(MachineSession& session, const AppPaths& paths, Events events);

    DiskLists diskLists() const;

    // Empty => no disk. A live hot-swap (the floppy isn't sized by the boot
    // ROM; a real drive lets you swap diskettes while powered). Only the
    // selection changes if no drive is attached.
    void selectDisk(const std::string& diskNameOrEmpty);
    const std::string& selectedDiskName() const { return m_diskName; }
    // Name & Save is offered only for a template disk.
    bool canNameAndSave() const;

    // Side A/B (0/1) -- ejecting and flipping the physical disk; re-arms the
    // drive's changed-disk latch.
    int side() const;
    void toggleSide();
    // The "green lamp": the drive motor is spinning.
    bool motorOn() const;

    // MachineSession::Bays::insertSelectedDisk(): loads the selected
    // disk into a just-attached drive.
    void insertSelectedDisk();
    // After a preset's `floppy:` loaded a disk directly on the machine.
    void syncFromPresetLoad(const std::string& labelOrEmpty, const std::string& resolvedPathOrEmpty = {});

    // "Name & Save" -- a bundled name is always refused.
    bool nameAndSave(const std::string& diskName, std::string* error);
    // Preset `saveas: template|live floppy:<name>`; see ModuleBay's.
    bool saveAsFromPreset(const std::string& diskName, const std::string& filePath, bool asTemplate,
                          std::string* error);

    // Once per frame: notices a changed disk and writes it once the
    // debounce has passed.
    void tick(double nowSeconds);
    // MachineSession::Bays::flushFloppy(), and before a select / quit.
    void flushPendingPersist();

private:
    PC1600Machine* drive() const;  // the PC-1600 if its CE-1600F is attached
    bool saveDiskAs(const std::string& diskName, bool fromPreset, std::string* error, const std::string& filePath = {},
                    bool asTemplate = false);
    bool nameCollides(const std::string& diskName) const;
    // A template is never written; an instance autosaves in place (never
    // into the bundle).
    void classifySource(const std::string& resolvedPathOrEmpty, bool isTemplate);
    bool loadSelectedDisk(PC1600Machine* m1600);
    void writeInstance();
    void reportError(const std::string& text) const {
        if (m_events.error) m_events.error(text);
    }

    MachineSession& m_session;
    const AppPaths& m_paths;
    Events m_events;
    std::string m_diskName;          // empty = no disk in the drive
    bool m_isTemplate = false;       // the loaded file declares `template: true`
    std::string m_instanceFilePath;  // the loaded file if it is an instance, else empty
    bool m_persistPending = false;
    std::uint64_t m_lastSeenRevision = 0;
    std::optional<double> m_persistDue;  // set while m_persistPending
};

} // namespace shared
