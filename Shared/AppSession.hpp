#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Job.hpp"
#include "Shared/AppPaths.hpp"
#include "Shared/AudioPump.hpp"
#include "Shared/ControlState.hpp"
#include "Shared/FloppyBay.hpp"
#include "Shared/MachineSession.hpp"
#include "Shared/ModuleBay.hpp"
#include "Shared/Pacer.hpp"
#include "Shared/Settings.hpp"
#include "Shared/TapeBay.hpp"

namespace shared {

// What an operation reports: ok, or why not (a sentence for the user).
struct OpResult {
    bool ok = true;
    std::string error;
    static OpResult fail(std::string why) { return OpResult{false, std::move(why)}; }
};

// File > Load Machine Code…: the bytes and where they go.
struct MachineCodeLoadRequest {
    std::vector<std::uint8_t> payload;
    std::uint32_t addr = 0;  // the Z-80 / LH5801 bus address
    int slot = 0;            // PC-1600: machinecode::Slot (0 = S0, 1 / 2 = memory slots)
    int bank = 0;            // PC-1600 slots: the global bank
};

// Which bay a message is about.
enum class Bay { Modules, Floppy, Tape };

// ── AppSession: the whole emulator behind one frontend ───────────────────
//
// Owns the live machine (MachineSession), the module,
// floppy and tape bays and the pacer, wires them to each other, and runs
// the app's operations: presets, resets, plotter power cycles, model and
// ROM switches. An operation is a resumable job (Core/Job.hpp); one runs
// at a time, and while it does, paced emulation stands still.
//
// A frontend:
//  - implements Platform (what only the host can do or show),
//  - calls tick(now) from its ~60 Hz frame timer and repaints,
//  - starts operations with start() (a browser: the job then runs a time
//    slice per tick) or runs them with runNow() (a desktop app that
//    blocks, pumping its event loop between slices).
//
// Single-threaded: everything is called on the frontend's one thread.
class AppSession : private MachineSession::Bays {
public:
    // The message box for Platform::attachFailed().
    static MachineSession::NoticeText attachFailedText(const std::string& device, const std::string& reason);

    // The machine's events (MachineSession::Listener: ROM missing, model
    // switched, debugger hooks, ...) plus the app's own.
    class Platform : public MachineSession::Listener {
    public:
        // A bay changed or failed outside the user's own pick.
        virtual void moduleChanged(int /*slot*/) {}
        virtual void tapesChanged() {}
        virtual void bayError(Bay /*bay*/, const std::string& /*text*/) {}

        // A preset's machine is armed (modules, plotter, disk attached and
        // the bays synced to them) but not yet booted: repaint it now.
        virtual void presetArmed() {}
        // One line of a preset's step log.
        virtual void presetLog(const std::string& /*line*/) {}
        // A peripheral the user asked for couldn't be attached; it stays
        // detached.
        // attachFailedText() words it.
        virtual void attachFailed(const std::string& /*device*/, const std::string& /*reason*/) {}
        // An operation started / finished (start() and runNow()).
        virtual void busyChanged(bool /*busy*/) {}

        // The host has directories the PC-1600 host drive can mount (a
        // browser has none): File ▸ Mount Directory is offered.
        virtual bool hostDirectories() const { return true; }
    };

    // `settings` and `paths` are the host's (it may need them before the
    // session exists, e.g. to isolate the save folder).
    AppSession(Settings& settings, AppPaths& paths, Platform& platform);
    ~AppSession() override;
    AppSession(const AppSession&) = delete;
    AppSession& operator=(const AppSession&) = delete;

    Settings& settings() { return m_settings; }
    AppPaths& paths() { return m_paths; }
    MachineSession& machine() { return *m_machine; }
    ModuleBay& modules() { return *m_modules; }
    FloppyBay& floppy() { return *m_floppy; }
    TapeBay& tapes() { return *m_tapes; }
    Pacer& pacer() { return m_pacer; }

    // What the pickers, menus and panels offer for the live machine
    // (Shared/ControlState.hpp).
    ControlState controlState() const { return controlStateOf(*m_machine, m_platform.hostDirectories()); }

    // Builds the first machine: the startup model from Settings, cold
    // booted. Call once, after the frontend is ready for Platform calls.
    void start();

    // ---- Operations (resumable; run one with start() or runNow()) ----
    // A preset file, end to end: parse, build the machine it targets, arm
    // it (the bays follow), boot, run its steps. `armOnly`: set up, not
    // booted. `ref` is a path or a bundled "examples:..." one
    // (AppPaths::resolvePresetRef()).
    job::Task<OpResult> loadPresetJob(std::string ref, bool armOnly = false);
    // A model's default preset: refuses -- touching nothing -- one that
    // targets another model than `model`.
    job::Task<OpResult> loadDefaultPresetJob(std::string ref, Model model);
    // Reset / Reset All, booted flat out to the prompt.
    job::Task<OpResult> resetJob(bool allReset);
    // A fresh default machine of `model`: A04 / new ROMs, no plotter, no
    // modules, no disk. Nothing if it already is that model. (The default
    // preset is a separate operation: loadDefaultPresetJob().)
    job::Task<OpResult> selectModelJob(Model model);
    // The current model's default preset (Settings::defaultPresetPath()),
    // if one is set -- at startup and after a model pick. Ok when none is.
    job::Task<OpResult> defaultPresetJob();
    // A model picked on the control bar / Machine menu: a fresh default
    // machine (selectModelJob()), then its default preset. Nothing at all
    // if it already is that model.
    job::Task<OpResult> pickModelJob(Model model);
    // A module picked for `slot` ("" = none): a rebuild that keeps the
    // attached peripherals, so the boot ROM sees the module.
    job::Task<OpResult> selectModuleJob(int slot, std::string moduleNameOrEmpty);
    // ROM picks: a rebuild that keeps the attached peripherals.
    job::Task<OpResult> selectPC1500RomJob(PC1500RomRevision revision);
    job::Task<OpResult> selectPC1600RomJob(PC1600RomVersion version);
    // The CE-1600P ROM sits in its box: on an attached one this is a box
    // swap inside an OFF/ON cycle; otherwise it records the choice.
    job::Task<OpResult> selectCE1600PRomJob(CE1600PRomVersion version);
    // Attaches or detaches `p` inside the OFF/ON power cycle real hardware
    // needs. Core decides what may share the bus (attaching one may drop
    // another). A failed attach goes to Platform::attachFailed.
    job::Task<OpResult> togglePeripheralJob(Peripheral p);
    // PC-1600 host drive S3: on `dir` (a power cycle if it isn't attached
    // yet, else a live directory swap), or detached ("").
    job::Task<OpResult> mountHostDriveJob(std::string dirOrEmpty);

    // Instant loads into the running machine (no reset, like LOAD).
    OpResult loadBasicProgram(const std::vector<std::uint8_t>& file, const std::string& path);
    OpResult loadMachineCode(const MachineCodeLoadRequest& request);

    // ---- Running operations ----
    using Done = std::function<void(const OpResult&)>;
    // Starts `op`; it runs a slice per tick(). False (and `op` dropped) if
    // another one is running.
    bool start(job::Task<OpResult> op, Done done = {});
    // Runs `op` now, a slice at a time, calling `pump` between slices (a
    // desktop app keeps painting). Inside another operation (a nested call)
    // it runs straight through.
    OpResult runNow(job::Task<OpResult> op, const std::function<void()>& pump = {});
    bool busy() const { return m_busy; }
    // Host time an operation slice may take before the frontend gets
    // control back (as Calc-U-59's full-speed slice).
    static constexpr double kOperationSliceSeconds = 0.010;

    // ---- Frame ----
    // The frontend's frame tick: a slice of the running operation, or
    // paced emulation (turbo if set on pacer()); then frameDuties() and the
    // buzzer audio (played if attachAudio() was called, thrown away when
    // the tick wasn't real time: turbo or an operation slice).
    void tick();

    // Scripted runs (frozen frontends, the --shots runner): `seconds` of
    // emulated time / until the paste queue is empty (false if it still
    // types after `capSeconds`), then frameDuties(). Not real time, so the
    // audio is thrown away.
    void runFor(double seconds);
    bool runUntilPasteDone(double capSeconds);

    // The host's audio output; it must outlive the session. Without one
    // the audio is drained and dropped. warmUpAudio(): AudioPump::warmUp().
    void attachAudio(AudioSink& sink, AudioPump::Warn warn);
    void warmUpAudio();
    // The tape deck as frameDuties() last read it (what the tape bay saw).
    const TapeDeck::Status& tapeStatus() const { return m_tapeStatus; }

private:
    // MachineSession::Bays
    void attachModulesToFreshMachine() override { m_modules->attachAllToFreshMachine(); }
    void flushFloppy() override { m_floppy->flushPendingPersist(); }
    void insertSelectedDisk() override { m_floppy->insertSelectedDisk(); }

    // Before an operation: nothing else may drive the machine, and pending
    // card/disk writes go first. After: the flat-out run beeped stale
    // audio, and its clock is off -- re-seed it and rebase pacing.
    void beginOperation();
    void endOperation();
    // Per-frame bookkeeping: battery-card and disk autosave, the tape bay.
    // Runs every tick() and at the end of every operation.
    void frameDuties();
    // After a run that wasn't real time (an operation, a scripted run):
    // frameDuties(), and its audio is thrown away.
    void finishUnpaced();
    // Steps `j` for one kOperationSliceSeconds slice; true once it's done.
    bool stepSlice(job::Job<OpResult>& j);
    // The finished job's result; a thrown exception becomes a failure.
    static OpResult takeResult(job::Job<OpResult>& j);
    double nowSeconds() const;

    Settings& m_settings;
    AppPaths& m_paths;
    Platform& m_platform;
    std::unique_ptr<MachineSession> m_machine;
    std::unique_ptr<ModuleBay> m_modules;
    std::unique_ptr<FloppyBay> m_floppy;
    std::unique_ptr<TapeBay> m_tapes;
    Pacer m_pacer;
    TapeDeck::Status m_tapeStatus;
    std::unique_ptr<AudioPump> m_audio;

    bool m_busy = false;
    job::Job<OpResult> m_op;
    Done m_opDone;
};

} // namespace shared
