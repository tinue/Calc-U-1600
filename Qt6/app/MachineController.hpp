#pragma once
#include <QObject>
#include <QString>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Connector/AlpsPlotterMechanism.hpp"
#include "Display/LcdScreenshot.hpp"
#include "KeyPaste.hpp"
#include "PC1500/PC1500Variant.hpp"
#include "TraceTypes.hpp"

class PC1500Machine;
class PC1600Machine;
class MemoryModuleManager;
class FloppyDiskManager;
class PtySerialLink;

namespace MachineControllerNS {
enum class Model { PC1500, PC1500A, PC1600 };
}
using MachineControllerNS::Model;

// The per-model settings key ("PC1500"/"PC1500A"/"PC1600") shared by
// AppSettings::startupModelPreference() and defaultPresetPath(); lowercased,
// it's also that model's preset file extension.
inline QString modelSettingsKey(Model model) {
    switch (model) {
        case Model::PC1500: return QStringLiteral("PC1500");
        case Model::PC1500A: return QStringLiteral("PC1500A");
        case Model::PC1600: return QStringLiteral("PC1600");
    }
    return QString();
}

// One flat, model-agnostic frame the UI paints from -- deliberately wider
// than either Core display type (PC1500Display's 156x7 + 14 named status
// bits vs PC1600DisplaySnapshot's 156x32 + 17-entry status array) so
// LcdWidget never needs to know which model is active.
struct DisplayFrame {
    int cols = 0;
    int rows = 0;
    std::vector<bool> pixels; // row-major, cols*rows entries
    bool poweredOn = true;    // PC1500Machine::isDisplayOn() / PC1600 clockEnabled
    std::vector<std::pair<std::string, bool>> statusSymbols;
};

// Flat mirror of PC1600Machine::PageTarget/DebugBankState (PC1600Machine.hpp)
// for the debug panel's "Dump Mem" address-map view -- decoupled from Core's
// own type the same way DisplayFrame decouples from PC1600DisplaySnapshot, so
// this header doesn't need PC1600Machine.hpp's full definition.
enum class DebugPageTarget {
    OpenBus, SystemRomLo, SystemRomHi, Bank3Rom, Bank3bRom, Bank6Rom,
    PeripheralRom, InternalRam, Slot1, Slot2,
};

struct DebugBankStateFrame {
    uint8_t port31 = 0, port28 = 0, port3c = 0;
    uint8_t pageABank = 0, pageBBank = 0, pageCBank = 0, pageDBank = 0;
    uint8_t slot2MapMode = 0;
    bool slot1MapRemapped = false;
    bool hiddenBasicRom = false;
    int slot1CardBank = -1, slot2CardBank = -1;
    int slot1CardBankCount = -1, slot2CardBankCount = -1;
    DebugPageTarget target[4]{};
    bool slotmapRedirect[4]{};
};

// Facade over PC1500Machine/PC1600Machine -- owns whichever one is active
// for the currently-selected model and exposes what the Qt UI needs
// (switch/reset/keys/display), plus a narrow construction-only surface for
// PresetController (resetBareForPreset*/finishPresetLoad below) -- nothing
// from Core's debug/trace/plotter surface. Deliberately NOT a shared
// virtual interface: the
// two Core machines have genuinely different constructors, reset levels,
// and display shapes, so a common interface would only force a lowest-
// common-denominator type (see PC1600Machine.hpp's own note on not
// reusing PC1500Machine's facade).
class MachineController : public QObject {
    Q_OBJECT
public:
    explicit MachineController(QObject* parent = nullptr);
    ~MachineController();

    void switchModel(Model model);
    Model currentModel() const { return m_model; }

    // PC-1500 (plain) only -- PC-1500A is A04-only (see PC1500Variant.hpp)
    // and PC-1600 has its own fixed ROM set, so this is a no-op change of
    // m_pc1500RomRevision alone in those two cases (no rebuild), but a full
    // switchModel()-style rebuild when a PC-1500 is (or becomes) active, so
    // the new ROM actually takes effect immediately.
    void setPC1500RomRevision(PC1500RomRevision revision);
    PC1500RomRevision pc1500RomRevision() const { return m_pc1500RomRevision; }

    // Reset (`allReset` = ALL RESET on the PC-1600; the PC-1500 has one
    // level), then run the boot flat out until the ROM waits at the prompt
    // -- including a plotter's power-on init -- and set the clock from the
    // host. Synchronous: the caller stops the frame timer around it (see
    // PresetController::resetLive()).
    void resetToPrompt(bool allReset);

    void pressKey(const std::string& name);
    void releaseKey(const std::string& name);
    void setOnKeyPressed(bool pressed);

    // SHIFT one-shot-latch choreography for a host-keyboard-typed
    // character that needs a host Shift to produce: press "shift", wait,
    // release "shift", wait, press baseName, wait, release baseName.
    // Fire-and-forget -- self-contained, nothing for the caller to release
    // later. PC-1600 only, called by MainWindow's PC-1600 branch -- see
    // enqueueShiftedKey() for the PC-1500 equivalent.
    void tapShiftedKey(const std::string& baseName);

    // PC-1500 live-typing keystroke buffer, so fast host typing can't
    // outrun the ROM's key-scan loop and drop characters -- forwards to
    // PC1500Machine::enqueueKey(), which owns the whole press/hold/
    // release/idle cycle at the ROM's own confirmed-reliable cadence (see
    // its doc comment). No-op (silently ignored) when PC-1600 is active --
    // PC-1600 keystroke buffering is out of scope for now; MainWindow's
    // PC-1600 branch never calls this.
    void enqueueKey(const std::string& name);

    // PC-1500 equivalent of tapShiftedKey(), via the same queue: Shift is
    // a one-shot latch on real hardware (confirmed in
    // PC1500BasicTyper.cpp's typeLine()) -- tapped immediately before the
    // base key, not held -- so queueing "shift" then baseName reproduces
    // that exact confirmed technique instead of tapShiftedKey()'s
    // wall-clock QTimer chain, which can interleave incorrectly under
    // fast typing.
    void enqueueShiftedKey(const std::string& baseName);

    // Edit > Paste Text: types `text` (UTF-8) into the active machine at the
    // ROM's own keystroke cadence -- see Core/KeyPaste.hpp. Nothing is
    // added or validated; unmappable characters are skipped; a line break
    // is ENTER (a single trailing one is dropped). Appends to a paste still
    // in progress. Driven from advance(), so it pauses/turbos with the
    // emulation. Any machine swap or reset cancels it.
    void pasteText(const std::string& text);
    bool pasteActive() const { return m_paste.active(); }
    // Drops the rest of the paste, releasing a key it holds down.
    void cancelPaste();

    // Edit > Copy Screen: the active display's dot matrix as a physically
    // sized greyscale image (Core/Display/LcdScreenshot.hpp) -- the same
    // pixels a preset's `- screenshot:` step writes. Empty (0x0) with no
    // machine.
    GrayImage currentScreenImage() const;

    void advance(std::uint64_t cyclesBudget);

    // Buzzer audio from whichever machine is active (mono int16 PCM at
    // PiezoSampler::kDefaultSampleRate) -- see AudioOutput. drainAudio()
    // returns how many samples it wrote into `out`; 0 with no machine.
    std::size_t drainAudio(std::int16_t* out, std::size_t max);
    void discardAudio();
    DisplayFrame currentDisplay() const;

    // Cycles-per-second of whichever machine is currently active --
    // MainWindow's frame timer divides this by its own tick rate to get
    // a per-tick cycle budget.
    double clockHz() const;

    // Wired up once by MainWindow right after constructing both objects
    // (raw, not owned -- breaks the construction-order cycle:
    // MemoryModuleManager's ctor needs a MachineController*, but
    // switchModel(), called from MachineController's OWN constructor,
    // needs to call back into the manager). Before this is set,
    // switchModel() simply skips the attach callback -- harmless, since
    // no module is selected yet at cold start.
    void setModuleManager(MemoryModuleManager* mgr) { m_moduleManager = mgr; }
    // The CE-1600F comes and goes with the CE-1600P: attachCE1600P() puts
    // the selected disk in, and anything that removes the drive
    // (detachCE1600P(), a PC-1600 attachCE150()) autosaves it first.
    void setFloppyManager(FloppyDiskManager* mgr) { m_floppyManager = mgr; }

    // Raw access for MemoryModuleManager to call the two different attach
    // APIs and read live card state -- kept as thin pass-throughs rather
    // than duplicating CardHost/attach logic inside this facade.
    PC1500Machine* pc1500() const { return m_pc1500.get(); }
    PC1600Machine* pc1600() const { return m_pc1600.get(); }

    // ---- Serial port (PC1600 only, SettingsDialog) ----
    // Re-reads AppSettings::serialLinkDirOverride() (falling back to
    // AppPaths::instanceDir()) and relinks the live PtySerialLink's stable
    // symlink to it. No-op if no PC1600Machine has been created yet -- the
    // link is created lazily on first PC-1600 activation and then kept
    // alive across model switches (see attachSerialLink() in the .cpp).
    void refreshSerialLinkDirectory();
    // The path a serial client should open (the stable symlink, or the raw
    // PTY slave if no symlink could be made), or empty when no PC-1600 has
    // ever been activated yet / the PTY failed to open.
    QString serialLinkStatus() const;

    // ---- Preset-loader support (PresetController only) ----
    // Replaces the live machine with a freshly constructed, UN-ROM'd
    // PC1500Machine for `variant` -- no ROM load, no module attach, no
    // reset. PC1500PresetLoader.cpp does all three itself (it loads the
    // ROM from a path the caller resolves, since a GUI has no repo-
    // relative roms/ directory -- see its header comment).
    PC1500Machine& resetBareForPresetPC1500(PC1500Variant variant);
    // Replaces the live machine with a freshly constructed PC1600Machine
    // with its fixed ROM set already loaded (same bytes/order as
    // switchModel()'s PC-1600 branch) but no module attach or reset --
    // PC1600PresetLoader.cpp does both itself, driven by the preset's own
    // slot1Module/slot2Module.
    PC1600Machine& resetBareForPresetPC1600();
    // Call once the preset loader returns, success or failure alike: the
    // machine object was already swapped in by resetBareForPreset*()
    // above -- this just finalizes model/UI bookkeeping the same way
    // switchModel() does at its end.
    void finishPresetLoad(Model model);

    // ---- Debug panel support (DebugPanel only) ----
    // Thin pass-throughs onto Core's already-existing debug/trace surface
    // (see PC1500Machine.hpp/PC1600Machine.hpp's own "debug reads" blocks).
    bool hasLiveMachine() const { return m_pc1500 || m_pc1600; }
    std::uint8_t debugPeek(std::uint16_t addr) const;

    // PC1500(A)-only ("Pointers" / "Dump Mem" / "Dump Card YAML"); harmless
    // no-op defaults (false / -1 / empty) when a PC1600 is active instead.
    bool debugSlotResponds(std::uint16_t addr) const;
    int debugSlotCardBank() const;
    int debugSlotCardBankCount() const;
    std::vector<std::uint8_t> debugSlotCardImage() const;

    // PC1600-only.
    bool slot1Attached() const;
    bool slot2Attached() const;
    std::vector<std::uint8_t> debugSlotImagePC1600(int slot) const;
    std::vector<std::uint8_t> debugInternalRamPC1600() const;
    DebugBankStateFrame debugBankStatePC1600() const;

    // Trace. setTraceEnabled()/traceEnabled() just flip the CPU trace-flag
    // switch; the three drain*Trace() calls hand back raw Core ring frames
    // for the caller (DebugPanel) to write into its own PC1500TraceFile --
    // kept as pass-throughs rather than owning a TraceFile here.
    void setTraceEnabled(bool enabled);
    bool traceEnabled() const;
    std::uint32_t drainPC1500Trace(CpuFrame* out, std::uint32_t max, std::uint32_t* outLost);
    std::uint32_t drainSC7852Trace(Z80CpuFrame* out, std::uint32_t max, std::uint32_t* outLost);
    std::uint32_t drainLH5803Trace(CpuFrame* out, std::uint32_t max, std::uint32_t* outLost);

    // ---- Plotter support (PlotterController/PlotterPaperWidget only) ----
    // Attach/detach are live calls into the already-running machine (NOT a
    // switchModel()-style rebuild) -- PlotterController wraps these in a
    // synthetic OFF/ON key sequence, matching real hardware's power-cycle
    // requirement. ROMs are resolved internally by Core's BundledRomCatalog
    // (same bundled resources directory loadPC1600RomSet() reads from), so
    // callers don't need to know any resource paths. Core already enforces
    // CE-150/CE-1600P mutual exclusion on the PC-1600's shared 60-pin bus
    // (PC1600Machine::attachCE1600P/attachCE150 each detach the other).
    bool attachCE150();   // works for PC1500(A) and PC1600 (LH5803 side)
    void detachCE150();
    bool ce150Attached() const;
    bool attachCE1600P(); // PC1600 only; false (no-op) otherwise
    void detachCE1600P();
    bool ce1600pAttached() const;

    std::vector<AlpsPlotterMechanism::FlatPoint> ce150PlotPoints() const;
    std::uint64_t ce150PlotRevision() const;
    void clearCE150Paper();
    std::vector<AlpsPlotterMechanism::FlatPoint> ce1600pPlotPoints() const;
    std::uint64_t ce1600pPlotRevision() const;
    void clearCE1600PPaper();

    // True while the active machine is powered on -- PC1600: sc7852Owns()
    // (false once the OFF-key/auto-power-off handoff parks it on the
    // LH5803 side); PC1500(A): !cpu().poweredOff(). PlotterController polls
    // this while waiting for a synthetic OFF keypress to actually land.
    bool isMachinePoweredOn() const;

signals:
    void modelChanged(Model model);

private:
    Model m_model = Model::PC1500A;
    PC1500RomRevision m_pc1500RomRevision = PC1500RomRevision::A04;
    std::unique_ptr<PC1500Machine> m_pc1500;
    std::unique_ptr<PC1600Machine> m_pc1600;
    KeyPasteFeeder m_paste;
    std::uint64_t m_pasteFrameCycles = 0; // cycles run since the paste feeder's last frame boundary
    void runActive(std::uint64_t cycles);
    void pasteOnFrame();
    MemoryModuleManager* m_moduleManager = nullptr; // not owned
    FloppyDiskManager* m_floppyManager = nullptr;   // not owned
    void flushFloppyBeforeDetach();

    // Lazily created the first time a PC1600Machine exists, then kept alive
    // for the rest of the app's life (see attachSerialLink()) -- a stable
    // host-visible symlink shouldn't disappear/reappear just because the
    // user switched models or loaded a preset.
    std::unique_ptr<PtySerialLink> m_serialLink;

    // Where Core's BundledRomCatalog should look for bundled ROM files --
    // AppPaths::bundledResourcesDir(), the same directory the .card.yaml
    // catalog lives in.
    static std::vector<std::string> bundledRomDirs();
    void loadPC1600RomSet(PC1600Machine& machine);
    void seedClockFromHost();
    // AppSettings::serialLinkDirOverride(), falling back to AppPaths::instanceDir().
    static QString effectiveSerialLinkDir();
    // Ensures m_serialLink exists (constructing it from the effective
    // AppSettings directory on first call) and attaches it to `machine` --
    // called after every point that (re)constructs m_pc1600.
    void attachSerialLink(PC1600Machine& machine);
};
