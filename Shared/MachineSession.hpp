#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Connector/AlpsPlotterMechanism.hpp"
#include "Debug/Inspect/Inspector.hpp"
#include "Display/LcdScreenshot.hpp"
#include "Display/LcdText.hpp"
#include "Job.hpp"
#include "KeyPaste.hpp"
#include "PC1500/PC1500Variant.hpp"
#include "Serial/LazySerialLink.hpp"
#include "Serial/PtySerialLink.hpp"
#include "Tape/TapeDeck.hpp"
#include "TraceTypes.hpp"
#include "Shared/AppPaths.hpp"
#include "Shared/DisplayFrame.hpp"
#include "Shared/HostKeyMap.hpp"
#include "Shared/MachineModel.hpp"
#include "Shared/PlotterPaper.hpp"
#include "Shared/Settings.hpp"

class PC1500Machine;
class PC1600Machine;

namespace shared {

// ── MachineSession ───────────────────────────────────────────────────────
//
// The live calculator behind every frontend: owns whichever of
// PC1500Machine / PC1600Machine is active for the selected model and
// everything around it -- model and ROM switches (each a rebuild and cold
// boot), keys and typing (paste queue, live KBII sequences, timed taps),
// the display, audio, plotters, CE-158, tape, host drive, trace and
// inspector reads. Deliberately NOT a shared virtual interface over the
// two machines: they have genuinely different constructors, reset levels
// and display shapes (see PC1600Machine.hpp's note on not reusing
// PC1500Machine's facade); withMachine() runs a generic lambda on the
// live one instead.
//
// Single-threaded: the frontend calls everything, including advance(),
// from one thread. What the frontend must do or show comes back through
// the Listener.
class MachineSession {
public:
    // Something worth telling the user that doesn't stop the session.
    enum class Notice {
        // The old PC-1600 ROM set could not be loaded; the new one is used.
        OldPC1600RomUnavailable,
        // The old CE-1600P ROM could not be loaded; the new one is used.
        OldCE1600PRomUnavailable,
        // Ejecting the tape before a rebuild failed to save the recording.
        TapeSaveFailed,
    };
    // A notice as a message box: its title and its sentence.
    struct NoticeText {
        std::string title;
        std::string text;
    };
    static NoticeText noticeText(Notice notice, const std::string& detail);

    class Listener {
    public:
        virtual ~Listener() = default;
        // A bundled ROM the active model needs is missing: the install is
        // broken and there is no machine to fall back to. Expected not to
        // return (the Qt app reports and exits); if it does, the session
        // throws std::runtime_error.
        virtual void romMissing(const std::string& error, const std::string& searchedIn) = 0;
        virtual void notice(Notice, const std::string& /*detail*/) {}

        // The live machine is about to be destroyed / a new one was just
        // built (before anything is attached). The debugger hooks in here.
        virtual void machineAboutToChange() {}
        virtual void machineReplaced() {}
        // switchModel() or finishPresetLoad() finished.
        virtual void modelSwitched(Model) {}

        // With a debugger attached, a frame's cycles go through its run
        // control (pause, stepping, breakpoints): return true when it ran
        // them itself.
        virtual bool runDebuggerSlice(std::uint64_t /*cycles*/) { return false; }

        // refreshSerialLinkDirectory() moved the PTY symlinks.
        virtual void serialLinksRelinked() {}
    };

    // The bays' part in a rebuild or a bus change (AppSession wires its
    // ModuleBay and FloppyBay in here; a session without bays has none).
    class Bays {
    public:
        virtual ~Bays() = default;
        // Attaches the selected modules to a machine just built, ahead of
        // the cold boot.
        virtual void attachModulesToFreshMachine() = 0;
        // The disk drive goes away (CE-1600P detach, machine rebuild, a
        // CE-150/CE-158 taking the bus): save the disk first.
        virtual void flushFloppy() = 0;
        // A CE-1600P was just attached: put the selected disk in.
        virtual void insertSelectedDisk() = 0;
    };

    MachineSession(Settings& settings, const AppPaths& paths, Listener& listener, Bays* bays = nullptr);
    ~MachineSession();
    MachineSession(const MachineSession&) = delete;
    MachineSession& operator=(const MachineSession&) = delete;

    // The long operations below (a rebuild's cold boot, a reset, a power
    // cycle) each come as a resumable job (Core/Job.hpp) and as the
    // blocking call of the same name that runs it straight through.

    // Back to the default ROMs (PC-1500 A04, PC-1600 new) without rebuilding;
    // called before a model switch, which starts from a default machine.
    void resetRomSelectionsToDefault();
    // Rebuilds the machine for `model` and cold boots it to the prompt.
    // keepPeripherals: re-attach whichever plotter (CE-150 / CE-1600P),
    // CE-158 and (PC-1600) host drive was attached before the rebuild,
    // ahead of the cold boot, so a rebuild (module or ROM change) never
    // detaches them as a side effect. Default false (model switch, preset
    // load: fresh machine, nothing attached).
    void switchModel(Model model, bool keepPeripherals = false);
    job::Task<void> switchModelJob(Model model, bool keepPeripherals = false);
    Model currentModel() const { return m_model; }

    // PC-1500 (plain) only -- PC-1500A is A04-only (see PC1500Variant.hpp)
    // and PC-1600 has its own version picker, so this only records the
    // revision in those cases, but rebuilds when a PC-1500 is active.
    void setPC1500RomRevision(PC1500RomRevision revision);
    job::Task<void> setPC1500RomRevisionJob(PC1500RomRevision revision);
    PC1500RomRevision pc1500RomRevision() const { return m_pc1500RomRevision; }
    // PC-1600 calculator ROM version (the CE-1600P ROM is independent of
    // it). Rebuilds the machine when a PC-1600 is active.
    void setPC1600RomVersion(PC1600RomVersion version);
    job::Task<void> setPC1600RomVersionJob(PC1600RomVersion version);
    PC1600RomVersion pc1600RomVersion() const { return m_pc1600RomVersion; }
    // CE-1600P ROM version (the CE-1600F follows it). Only records the
    // choice for the next attach -- see swapCE1600PRom().
    void setCE1600PRomVersion(CE1600PRomVersion version) { m_ce1600pRomVersion = version; }
    CE1600PRomVersion ce1600pRomVersion() const { return m_ce1600pRomVersion; }

    // Reset (`allReset` = ALL RESET on the PC-1600; the PC-1500 has one
    // level), then run the boot flat out until the ROM waits at the prompt
    // -- including a plotter's power-on init. Synchronous.
    void resetToPrompt(bool allReset);
    job::Task<void> resetToPromptJob(bool allReset);
    // Sets the live machine's clock from the host. Called when paced
    // emulation (re)starts: every flat-out run before it -- a boot, a
    // preset, a power cycle -- went ahead of (or behind) the host clock.
    void seedClockFromHost();
    // The attach/detach power cycle, flat out: OFF, wait for the ROM to
    // power down, `change()` (the instantaneous attach or detach), ON, boot
    // to the prompt (incl. a plotter's power-on init). A machine that is
    // already off just gets `change()`.
    void powerCycleAround(const std::function<void()>& change);
    job::Task<void> powerCycleAroundJob(std::function<void()> change);

    // ---- Expansion slots ----
    // The memory-module slots of the live machine: 1 on the PC-1500/1500A
    // (the 40-pin connector), 2 on the PC-1600 (Slot 1 / Slot 2); 0 with
    // no machine. Slots count from 1.
    int slotCount() const;
    // The card in `slot`, null for an empty or missing slot.
    const ExpansionCard* slotCard(int slot) const;
    // Takes ownership of `card` and plugs it into `slot` (ignored for a
    // missing slot); detachSlots() empties every slot.
    void attachSlotCard(int slot, std::unique_ptr<ExpansionCard> card);
    void detachSlots();

    // ---- Host keyboard ----
    // A host key resolved to a calculator key (Shared/HostKeyMap). Stops a
    // paste, then types it the way the model needs: on the PC-1600 an
    // accented character (or any key while one still types) through the
    // live queue, a shifted character as a Shift tap sequence, a plain
    // `physical` key as a real press, a committed character as a tap; on
    // the PC-1500 the cursor keys pressed for real (the ROM's own
    // auto-repeat), everything else through the keystroke queue. True if
    // it pressed a key the frontend must release with hostKeyUp() when
    // the host key comes up.
    bool hostKeyDown(const ResolvedKey& key, bool physical);
    void hostKeyUp(const std::string& baseKey) { releaseKey(baseKey); }
    // A bare Shift press and release: one calculator SHIFT tap (behind a
    // live sequence still typing). Stops a paste.
    void hostShiftTap();

    // ---- Keys and typing ----
    // A frontend's own key (faceplate, physical host key). A release that
    // comes before kMinKeyHoldFrames (2) of emulated time since the press -- a
    // quick finger tap, both events between two advance() calls -- is
    // deferred until then, so the ROM's key scan always sees the key; a
    // press while such a release is still to come waits for it.
    void pressKey(const std::string& name);
    void releaseKey(const std::string& name);
    void setOnKeyPressed(bool pressed);
    // PC-1600: SHIFT one-shot-latch choreography for a host character that
    // needs Shift: press shift, 30 ms, release, 100 ms, press baseName,
    // 30 ms, release -- in emulated time (advance()'s budget).
    void tapShiftedKey(const std::string& baseName);
    // One tap of `name`: through the live-typing queue on the PC-1500 (see
    // enqueueKey()), a 30 ms press on the PC-1600.
    void tapKey(const std::string& name);

    // Edit > Paste Text: types `text` (UTF-8) at the ROM's own keystroke
    // cadence -- see Core/KeyPaste.hpp. Unmappable characters are skipped;
    // a line break is ENTER (a single trailing one is dropped). Appends to
    // a paste in progress. Driven from advance(); any machine swap or reset
    // cancels it.
    void pasteText(const std::string& text);
    // The inbound counterpart for tools (the debugger's auto-start): types
    // `line`, then ENTER. Shares the paste queue.
    void typeCommand(const std::string& line);
    bool pasteActive() const { return m_paste.active() && !m_liveTyping; }
    // Drops the rest of the paste, releasing a key it holds down.
    void cancelPaste();
    bool liveTypingActive() const { return m_paste.active() && m_liveTyping; }

    // ---- Running ----
    // Runs `cyclesBudget` emulated cycles: in 60 Hz frames while a paste
    // types (the feeder acts at frame boundaries), through the debugger's
    // run control when one is attached; then fires due timed taps.
    void advance(std::uint64_t cyclesBudget);
    // Cycles-per-second of the active machine.
    double clockHz() const;

    // ---- Output ----
    DisplayFrame currentDisplay() const;
    // The LCD as text (Core/Display/LcdText.hpp); false with no machine.
    bool lcdText(LcdText* out) const;
    // Edit > Copy Screen: the dot matrix as a physically sized greyscale
    // image (the same pixels a preset's `- screenshot:` writes).
    GrayImage currentScreenImage() const;
    // Buzzer audio (mono int16 PCM at PiezoSampler::kDefaultSampleRate):
    // returns how many samples it wrote into `out`.
    std::size_t drainAudio(std::int16_t* out, std::size_t max);
    void discardAudio();

    // ---- The machines ----
    PC1500Machine* pc1500() const { return m_pc1500.get(); }
    PC1600Machine* pc1600() const { return m_pc1600.get(); }
    bool hasLiveMachine() const { return m_pc1500 || m_pc1600; }
    // True while the active machine is powered on.
    bool isMachinePoweredOn() const;
    // Runs `f` on whichever machine is live (both share the method names
    // used here); the second form returns `none` when there is none.
    template <class F>
    void withMachine(F&& f) const {
        if (m_pc1600) f(*m_pc1600);
        else if (m_pc1500) f(*m_pc1500);
    }
    template <class R, class F>
    R withMachine(R none, F&& f) const {
        if (m_pc1600) return f(*m_pc1600);
        if (m_pc1500) return f(*m_pc1500);
        return none;
    }
    // Where Core's BundledRomCatalog looks for bundled ROM files.
    std::vector<std::string> bundledRomDirs() const;

    // ---- Serial ports (PTYs; inert where the host has none) ----
    // Re-reads the serial link directory setting and relinks both PTYs'
    // stable symlinks there.
    void refreshSerialLinkDirectory();
    // The path a serial client should open (the stable symlink, or the raw
    // PTY), or empty while there is none.
    std::string serialLinkStatus() const;
    std::string ce158SerialLinkStatus() const;

    // ---- Preset loaders ----
    // Replaces the live machine with a fresh, UN-ROM'd PC1500Machine for
    // `variant` -- the preset loader loads the ROM, attaches and resets.
    PC1500Machine& resetBareForPresetPC1500(PC1500Variant variant);
    // Replaces the live machine with a fresh PC1600Machine holding the ROM
    // set of `version` (an unavailable old set falls back to the new one)
    // but no module attach or reset.
    PC1600Machine& resetBareForPresetPC1600(PC1600RomVersion version, CE1600PRomVersion ce1600pVersion);
    // Call once the preset loader returns, success or failure alike.
    void finishPresetLoad(Model model);

    // ---- Debug panel ----
    std::vector<std::string> inspectorView(inspect::View view) const;
    std::vector<inspect::MenuEntry> inspectorMenu(bool dumps) const;
    // The slot's card image (empty for none) and bank count (-1 = unbanked).
    std::vector<std::uint8_t> debugSlotCardImage(int slot, int* bankCount) const;
    // A full instruction trace to `path`; false if the file can't be opened
    // or a capture already runs. A machine rebuild ends the capture.
    bool beginTrace(const std::string& path);
    void endTrace();
    std::uint64_t traceBytes() const;
    bool traceActive() const;

    // ---- Peripherals ----
    // Live attach/detach on the running machine (run them inside
    // powerCycleAround()). ROMs come from the bundled resources. Core
    // enforces what shares the bus (the CE-150, CE-158 and CE-1600P on a
    // PC-1600). Attach failures give the reason in `error`.
    bool attachCE150(std::string* error = nullptr);
    void detachCE150();
    bool ce150Attached() const;
    bool attachCE1600P(std::string* error = nullptr);  // PC-1600 only
    void detachCE1600P();
    // Swaps the attached CE-1600P for one with the `version` ROM; the disk
    // moves over with the same side up.
    bool swapCE1600PRom(CE1600PRomVersion version, std::string* error = nullptr);
    bool ce1600pAttached() const;
    bool attachCE158(std::string* error = nullptr);
    void detachCE158();
    bool ce158Attached() const;
    std::vector<std::uint8_t> drainCE158PrinterOutput();

    // Host-directory drive S3: (PC-1600). Attaching a drive that isn't
    // attached needs powerCycleAround(); with it attached only the
    // directory changes, live. Survives a same-model rebuild, not a model
    // switch.
    bool attachHostDrive(const std::string& dir, std::string* error = nullptr);
    void detachHostDrive();
    bool hostDriveAttached() const;
    std::string hostDriveDirectory() const;  // empty = none

    // Cassette recorder on the tape interface's jacks (CE-150 on a
    // PC-1500(A), CE-1600P on a PC-1600). A recording is saved each time
    // the motor stops; a rebuild ejects the tape.
    bool tapeInterfaceAttached() const;
    bool tapePlay(const std::string& path, std::string* error = nullptr);
    void tapeRecord(const std::string& path);
    bool tapeEject(std::string* error = nullptr);
    TapeDeck::Status tapeStatus() const;
    std::string tapeTakeLastError();  // a failed automatic save, once

    // The plotter behind `kind` (ControlState::paper); None has no points.
    std::vector<AlpsPlotterMechanism::FlatPoint> plotPoints(PaperKind kind) const;
    std::uint64_t plotRevision(PaperKind kind) const;
    void clearPaper(PaperKind kind);

private:
    // ---- Live typing (hostKeyDown()/hostShiftTap() route here) ----
    // PC-1500 live-typing keystroke buffer, so fast host typing can't
    // outrun the ROM's key-scan loop and drop characters --
    // PC1500Machine::enqueueKey() owns the press/hold/release/idle cycle.
    // Ignored on the PC-1600.
    void enqueueKey(const std::string& name);
    // PC-1500 equivalent of tapShiftedKey(): Shift is a one-shot latch on
    // real hardware, tapped immediately before the base key.
    void enqueueShiftedKey(const std::string& baseName);
    // A live keystroke stops a paste: like cancelPaste(), but an accented
    // character's KBII sequence under way is finished so KBII isn't left
    // latched.
    void interruptPaste();
    // PC-1600 live host keys that need a key *sequence* (an accented
    // character, or any key typed while one still runs), queued on the
    // paste feeder's frame-paced cadence.
    void typeLiveStep(const PasteStep& step);

    Settings& m_settings;
    const AppPaths& m_paths;
    Listener& m_listener;
    Bays* m_bays;

    Model m_model = Model::PC1500A;
    PC1500RomRevision m_pc1500RomRevision = PC1500RomRevision::A04;
    PC1600RomVersion m_pc1600RomVersion = PC1600RomVersion::New;
    CE1600PRomVersion m_ce1600pRomVersion = CE1600PRomVersion::New;

    // The links outlive the machines (a stable host-visible symlink
    // shouldn't come and go with a model switch), so they're declared
    // first and destroyed last.
    // The PC-1600's RS-232C PTY: created the first time a PC1600Machine
    // exists, then kept for the session's life.
    std::unique_ptr<PtySerialLink> m_serialLink;
    // The CE-158's link, handed to every machine as it is built. Its PTY
    // only opens once a CE-158 first talks to it.
    LazySerialLink<PtySerialLink> m_ce158SerialLink;

    std::unique_ptr<PC1500Machine> m_pc1500;
    std::unique_ptr<PC1600Machine> m_pc1600;

    KeyPasteFeeder m_paste;
    bool m_liveTyping = false;             // m_paste holds live keystrokes (typeLiveStep()), not a paste
    std::uint64_t m_pasteFrameCycles = 0;  // cycles run since the paste feeder's last frame boundary

    // tapShiftedKey()/tapKey()'s PC-1600 presses and releases and
    // releaseKey()'s deferred releases, due at a point of m_budgetClock
    // (the sum of advance() budgets).
    struct TimedKey {
        std::uint64_t due;
        bool press;
        std::string name;
    };
    std::deque<TimedKey> m_timedKeys;
    std::uint64_t m_budgetClock = 0;
    void scheduleKey(double afterSeconds, bool press, const std::string& name);
    void scheduleKeyAt(std::uint64_t due, bool press, const std::string& name);
    void fireDueKeys();
    // pressKey()'s m_budgetClock per key still down, for releaseKey()'s
    // minimum hold.
    std::map<std::string, std::uint64_t> m_keyPressedAt;
    // The machine's key matrix, now: the timed taps and the paste feeder,
    // whose holds are their own.
    std::uint64_t minKeyHoldCycles() const;
    void machinePressKey(const std::string& name);
    void machineReleaseKey(const std::string& name);

    void enqueueTyped(const std::string& text, bool pressEnter);
    void runActive(std::uint64_t cycles);
    void pasteOnFrame();
    // Drops the live machine before a rebuild: ejects the tape (saving a
    // recording), ends a TRACE capture, cancels typing, destroys it.
    void discardMachine();
    // Hands a just-built machine its serial links and tells the listener.
    void wireNewMachine();
    // What switchModelJob(keepPeripherals) carries over a rebuild.
    struct Peripherals {
        bool ce150 = false;
        bool ce1600p = false;
        bool ce158 = false;
        std::string hostDriveDir;  // empty = none
    };
    Peripherals attachedPeripherals() const;
    void reattach(const Peripherals& p);
    // Every change that can take the CE-1600P (and its disk drive) off the
    // bus -- a rebuild, its detach, a CE-150/CE-158 taking its place --
    // runs through here: the disk is saved first.
    template <class F>
    auto busChange(F&& change) {
        if (m_bays && ce1600pAttached()) m_bays->flushFloppy();
        return std::forward<F>(change)();
    }
    // The serial link directory setting, falling back to the save folder.
    [[noreturn]] void romMissing(const std::string& error);
    // Replaces m_pc1600 with a fresh machine holding m_pc1600RomVersion's
    // ROM set. A missing old set falls back to (and remembers) the new one.
    void makePC1600WithRomFallback();
};

} // namespace shared
