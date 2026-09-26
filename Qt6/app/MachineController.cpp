#include "MachineController.hpp"

#include <cstdio>
#include <cstdlib>

#include <QDebug>
#include <QMessageBox>
#include <QTimer>

#include <algorithm>

#include "PC1500/PC1500BasicTyper.hpp"
#include "PC1500/PC1500Machine.hpp"
#include "PC1500/PC1500Screenshot.hpp"
#include "PC1500/PC1500TypedInput.hpp"
#include "PC1600/PC1600BasicTyper.hpp"
#include "PC1600/PC1600Machine.hpp"
#include "PC1600/PC1600Screenshot.hpp"
#include "PC1600/PC1600TypedInput.hpp"
#include "Resources/BundledRomCatalog.hpp"
#include "HostClock.hpp"
#include "AppPaths.hpp"
#include "AppSettings.hpp"
#include "FloppyDiskManager.hpp"
#include "debug/DebugController.hpp"
#include "MemoryModuleManager.hpp"

namespace {

const char* pc1500RomVariantName(PC1500RomRevision revision) {
    switch (revision) {
        case PC1500RomRevision::A01: return "A01";
        case PC1500RomRevision::A03: return "A03";
        case PC1500RomRevision::A04: return "A04";
    }
    return "A04";
}

// Which model to cold-boot into, per AppSettings::startupModelPreference():
// a pinned model, or ("last", the default) whatever was last switched to.
Model initialModel() {
    const QString pref = AppSettings::startupModelPreference();
    for (Model m : {Model::PC1500, Model::PC1500A, Model::PC1600}) {
        if (pref == modelSettingsKey(m)) return m;
    }
    return static_cast<Model>(AppSettings::lastUsedModel());
}

// A missing/unreadable bundled ROM means the install is broken -- there's
// no usable machine to fall back to (this is called both at startup and
// from a later model switch, and either way the app can't proceed without
// its firmware). Previously this was qFatal(), which aborts (SIGABRT) --
// on a packaged macOS/Windows build launched normally (not from a
// terminal) that's just a silent crash/bounce with no visible message at
// all. Show the user what's actually wrong and where Calc-U-1600 looked,
// then exit cleanly instead. std::exit() (not returning to unwind the
// call stack) is deliberate: this can be reached mid-construction of
// MainWindow/MachineController, which aren't set up to unwind safely from
// here, and terminating the process reclaims everything the OS owns
// regardless.
[[noreturn]] void reportMissingRomAndExit(const std::string& err) {
    const QString searchedIn = AppPaths::bundledResourcesDir();
    const QString message =
        QObject::tr("Calc-U-1600 could not start:\n\n%1\n\nSearched in: %2\n\n"
                     "If you built this from source, run tools/fetch_roms.sh to "
                     "download the required ROM files. If you're running an "
                     "installed copy, the app's ROM files may be missing or "
                     "corrupted -- try reinstalling.")
            .arg(QString::fromStdString(err), searchedIn);
    QMessageBox::critical(nullptr, QObject::tr("Missing ROM File"), message);
    std::exit(1);
}

} // namespace

MachineController::MachineController(QObject* parent) : QObject(parent) {
    m_debug = std::make_unique<DebugController>(this);
    switchModel(initialModel());
    m_debug->refreshServer();
}

MachineController::~MachineController() = default;

void MachineController::switchModel(Model model, bool keepPlotter) {
    const bool restoreCE150 = keepPlotter && ce150Attached();
    const bool restoreCE1600P = keepPlotter && ce1600pAttached();
    const bool restoreCE158 = keepPlotter && ce158Attached();
    flushFloppyBeforeDetach(); // the old machine (and its disk) is going away
    discardMachine();
    m_model = model;

    if (model == Model::PC1600) {
        makePC1600WithRomFallback();
        wireNewMachine();

        // Attach any currently-selected memory modules before the cold
        // boot -- a module's state must be visible on the very first ROM
        // check.
        if (m_moduleManager) m_moduleManager->attachAllToFreshMachine();
        if (restoreCE150) attachCE150();
        if (restoreCE1600P) attachCE1600P();
        if (restoreCE158) attachCE158();
    } else {
        const auto variant = (model == Model::PC1500A) ? PC1500Variant::PC1500A : PC1500Variant::PC1500;
        // PC-1500A is A04-only (PC1500Variant.hpp) -- clamp regardless of
        // whatever revision was last picked for the plain PC-1500.
        if (variant == PC1500Variant::PC1500A) m_pc1500RomRevision = PC1500RomRevision::A04;
        m_pc1500 = std::make_unique<PC1500Machine>(variant);
        wireNewMachine();

        std::string err;
        if (!BundledRoms::loadPC1500Rom(*m_pc1500, pc1500RomVariantName(m_pc1500RomRevision),
                                        bundledRomDirs(), &err)) {
            reportMissingRomAndExit(err);
        }

        if (m_moduleManager) m_moduleManager->attachAllToFreshMachine();
        if (restoreCE150) attachCE150();
        if (restoreCE158) attachCE158();
    }

    // Every rebuild is a full cold boot, run flat out to the prompt (incl. a
    // plotter's power-on init); the caller restarts paced emulation after
    // it, which sets the clock from the host (see seedClockFromHost()).
    // PC1500Machine has only one reset level, so only the PC-1600 gets the
    // ALL RESET; a freshly-constructed machine's RAM is cleared either way.
    resetToPrompt(/*allReset=*/model == Model::PC1600);
    discardAudio(); // whatever the flat-out boot beeped is stale

    AppSettings::setLastUsedModel(static_cast<int>(m_model));
    emit modelChanged(m_model);
}

void MachineController::setPC1600RomVersion(PC1600RomVersion version) {
    m_pc1600RomVersion = version;
    if (m_model == Model::PC1600) switchModel(m_model, /*keepPlotter=*/true); // rebuild with the new ROM
}

bool MachineController::setCE1600PRomVersion(CE1600PRomVersion version) {
    m_ce1600pRomVersion = version;
    if (m_model != Model::PC1600 || !ce1600pAttached()) return false;
    switchModel(m_model, /*keepPlotter=*/true); // rebuild with the new CE-1600P ROM
    return true;
}

void MachineController::setPC1500RomRevision(PC1500RomRevision revision) {
    m_pc1500RomRevision = revision;
    if (m_model != Model::PC1600) switchModel(m_model, /*keepPlotter=*/true); // rebuild with the new ROM
}

std::vector<std::string> MachineController::bundledRomDirs() {
    return {AppPaths::bundledResourcesDir().toStdString()};
}

bool MachineController::loadPC1600RomSet(PC1600Machine& machine, std::string* error) {
    const char* version = m_pc1600RomVersion == PC1600RomVersion::Old ? "old" : "new";
    return BundledRoms::loadPC1600RomSet(machine, bundledRomDirs(), version, error);
}

void MachineController::makePC1600WithRomFallback() {
    std::string romErr;
    for (;;) {
        m_pc1600 = std::make_unique<PC1600Machine>();
        if (loadPC1600RomSet(*m_pc1600, &romErr)) return;
        // Only the old ROM set is optional (its dump may be missing or
        // incomplete): warn, fall back to the new ROM and retry once --
        // a new-ROM failure quits. reportMissingRomAndExit() is
        // [[noreturn]], so the retry can't loop a third time.
        if (m_pc1600RomVersion != PC1600RomVersion::Old) reportMissingRomAndExit(romErr);
        QMessageBox::warning(nullptr, QObject::tr("Old ROM unavailable"),
                             QObject::tr("The old PC-1600 ROM could not be loaded:\n\n%1\n\n"
                                         "Using the new ROM instead.")
                                 .arg(QString::fromStdString(romErr)));
        m_pc1600RomVersion = PC1600RomVersion::New;
    }
}

PC1500Machine& MachineController::resetBareForPresetPC1500(PC1500Variant variant) {
    discardMachine();
    m_pc1500 = std::make_unique<PC1500Machine>(variant);
    wireNewMachine(); // a preset's `interface: ce158` picks up the link from here
    return *m_pc1500;
}

PC1600Machine& MachineController::resetBareForPresetPC1600(PC1600RomVersion version,
                                                           CE1600PRomVersion ce1600pVersion) {
    m_pc1600RomVersion = version;
    m_ce1600pRomVersion = ce1600pVersion;
    discardMachine();
    makePC1600WithRomFallback();
    wireNewMachine(); // see resetBareForPresetPC1500
    return *m_pc1600;
}

QString MachineController::effectiveSerialLinkDir() {
    const QString dir = AppSettings::serialLinkDirOverride();
    return dir.isEmpty() ? AppPaths::instanceDir() : dir;
}

void MachineController::attachSerialLink(PC1600Machine& machine) {
    if (!m_serialLink) {
        m_serialLink = std::make_unique<PtySerialLink>(effectiveSerialLinkDir().toStdString());
    }
    machine.setSerialLink(m_serialLink.get());
}

void MachineController::refreshSerialLinkDirectory() {
    const std::string dir = effectiveSerialLinkDir().toStdString();
    if (m_serialLink) m_serialLink->relink(dir);
    if (PtySerialLink* ce158 = m_ce158SerialLink.link()) ce158->relink(dir);
    emit serialLinksMoved();
}

QString MachineController::serialLinkStatus() const {
    if (!m_serialLink || !m_serialLink->isOpen()) return QString();
    return QString::fromStdString(m_serialLink->preferredPath());
}

QString MachineController::ce158SerialLinkStatus() const {
    const PtySerialLink* link = m_ce158SerialLink.link();
    if (!link || !link->isOpen()) return QString();
    return QString::fromStdString(link->preferredPath());
}

void MachineController::wireNewMachine() {
    withMachine([this](auto& machine) { machine.setCE158SerialLink(&m_ce158SerialLink); });
    if (m_pc1600) attachSerialLink(*m_pc1600);
    if (m_debug) m_debug->machineReplaced();
}

void MachineController::finishPresetLoad(Model model) {
    m_model = model;
    AppSettings::setLastUsedModel(static_cast<int>(m_model));
    emit modelChanged(m_model);
}

void MachineController::seedClockFromHost() {
    withMachine([](auto& machine) { seedClockFromHostTime(machine); });
}

void MachineController::resetToPrompt(bool allReset) {
    cancelPaste();
    withMachine([allReset](auto& machine) {
        if (allReset) machine.allReset();
        else machine.reset();
        runBootToPrompt(machine);
    });
}

namespace {

// ~4 frames @60Hz: long enough for the ROM's key-scan loop to see the press.
constexpr int kKeyHoldFrames = 4;
// 3 s of emulated time: don't hang if the power-down path never completes.
constexpr int kPowerOffTimeoutFrames = 180;

}  // namespace

void MachineController::powerCycleAround(const std::function<void()>& change) {
    if (!isMachinePoweredOn()) {
        change();  // already off: no synthetic power cycle needed
        return;
    }
    cancelPaste();
    const auto frame = static_cast<std::uint64_t>(clockHz() / 60);
    withMachine([&](auto& machine) {
        machine.pressKey("off");
        machine.runCycles(frame * kKeyHoldFrames);
        machine.releaseKey("off");
        for (int i = 0; i < kPowerOffTimeoutFrames && isMachinePoweredOn(); ++i) machine.runCycles(frame);
        change();
        machine.setOnKeyPressed(true);
        machine.runCycles(frame * kKeyHoldFrames);
        machine.setOnKeyPressed(false);
        runBootToPrompt(machine);
    });
}

void MachineController::pressKey(const std::string& name) {
    withMachine([&](auto& machine) { machine.pressKey(name); });
}

void MachineController::releaseKey(const std::string& name) {
    withMachine([&](auto& machine) { machine.releaseKey(name); });
}

void MachineController::setOnKeyPressed(bool pressed) {
    withMachine([&](auto& machine) { machine.setOnKeyPressed(pressed); });
}

void MachineController::tapShiftedKey(const std::string& baseName) {
    // 30ms shift hold, 100ms gap between taps, 30ms base-key hold.
    pressKey("shift");
    QTimer::singleShot(30, this, [this] { releaseKey("shift"); });
    QTimer::singleShot(30 + 100, this, [this, baseName] { pressKey(baseName); });
    QTimer::singleShot(30 + 100 + 30, this, [this, baseName] { releaseKey(baseName); });
}

void MachineController::tapKey(const std::string& name) {
    if (m_pc1500) {
        m_pc1500->enqueueKey(name);
    } else if (m_pc1600) {
        pressKey(name);
        QTimer::singleShot(30, this, [this, name] { releaseKey(name); });
    }
}

void MachineController::enqueueKey(const std::string& name) {
    if (m_pc1500) {
        m_pc1500->enqueueKey(name);
    }
    // PC-1600 buffering is out of scope for now -- silently ignored,
    // matching pressKey()/releaseKey()'s existing convention for a
    // name/model combination that doesn't apply.
}

void MachineController::enqueueShiftedKey(const std::string& baseName) {
    enqueueKey("shift");
    enqueueKey(baseName);
}

void MachineController::pasteText(const std::string& text) { enqueueTyped(text, /*pressEnter=*/false); }

void MachineController::typeCommand(const std::string& line) { enqueueTyped(line, /*pressEnter=*/true); }

void MachineController::enqueueTyped(const std::string& text, bool pressEnter) {
    if (!m_pc1500 && !m_pc1600) return;
    if (!m_paste.active()) m_pasteFrameCycles = 0;
    std::vector<PasteStep> steps = buildPasteSteps(text, m_pc1600 ? pc1600ResolveTypedChar : pc1500ResolveTypedChar);
    if (pressEnter) steps.push_back(PasteStep{"enter", false});
    m_paste.setPacing(m_pc1600 ? pc1600PastePacing() : pc1500PastePacing());
    m_paste.append(steps);
}

void MachineController::cancelPaste() {
    m_paste.cancel([this](const std::string& key) { releaseKey(key); });
}

GrayImage MachineController::currentScreenImage() const {
    if (m_pc1600) return renderLcdImage(pc1600LcdBitmap(*m_pc1600), kPC1600ScreenMm);
    if (m_pc1500) return renderLcdImage(pc1500LcdBitmap(*m_pc1500), kPC1500ScreenMm);
    return GrayImage{};
}

void MachineController::runActive(std::uint64_t cycles) {
    // With a debugger attached, the frame's budget goes through its run
    // control (pause, stepping, breakpoints).
    if (m_debug && m_debug->attached()) {
        m_debug->runSlice(cycles);
        return;
    }
    withMachine([&](auto& machine) { machine.runCycles(cycles); });
}

void MachineController::refreshDebugServer() { m_debug->refreshServer(); }

void MachineController::pasteOnFrame() {
    m_paste.onFrame([this](const std::string& key) { pressKey(key); },
                    [this](const std::string& key) { releaseKey(key); });
}

void MachineController::advance(std::uint64_t cyclesBudget) {
    // While a paste is typing, run in 60 Hz emulated frames and let the
    // feeder act at each frame boundary (its cadence is counted in frames,
    // so it holds under wall-clock pacing and turbo alike).
    const auto frameCycles = static_cast<std::uint64_t>(clockHz() / 60.0);
    while (m_paste.active() && cyclesBudget > 0 && frameCycles > 0) {
        const std::uint64_t chunk = std::min(cyclesBudget, frameCycles - m_pasteFrameCycles);
        runActive(chunk);
        cyclesBudget -= chunk;
        m_pasteFrameCycles += chunk;
        if (m_pasteFrameCycles >= frameCycles) {
            m_pasteFrameCycles = 0;
            pasteOnFrame();
        }
    }
    if (cyclesBudget > 0) runActive(cyclesBudget);
}

std::size_t MachineController::drainAudio(std::int16_t* out, std::size_t max) {
    return withMachine(std::size_t{0}, [&](auto& machine) { return machine.drainAudio(out, max); });
}

void MachineController::discardAudio() {
    withMachine([](auto& machine) { machine.discardAudio(); });
}

double MachineController::clockHz() const {
    if (m_pc1600) {
        return static_cast<double>(PC1600Machine::kTStateHz);
    }
    return static_cast<double>(PC1500Machine::kCpuHz);
}

DisplayFrame MachineController::currentDisplay() const {
    DisplayFrame frame;
    if (m_pc1600) {
        const PC1600DisplaySnapshot snap = m_pc1600->displaySnapshot();
        frame.cols = PC1600Display::kWidth;
        frame.rows = PC1600Display::kHeight;
        frame.pixels.resize(static_cast<std::size_t>(frame.cols) * frame.rows);
        for (int row = 0; row < frame.rows; ++row) {
            for (int col = 0; col < frame.cols; ++col) {
                frame.pixels[static_cast<std::size_t>(row) * frame.cols + col] = snap.pixels[row][col];
            }
        }
        frame.poweredOn = snap.clockEnabled;

        static const char* kSymbolNames[] = {
            "BUSY", "SHIFT", "S", "KBII", "SMALL", "DEG", "RAD", "GRAD",
            "RUN", "PRO", "RESERVE", "DEF", "I", "II", "III", "CTRL", "BATT",
        };
        for (std::size_t i = 0; i < PC1600StatusLine::kCount; ++i) {
            frame.statusSymbols.emplace_back(kSymbolNames[i], snap.statusSymbols[i]);
        }
    } else if (m_pc1500) {
        const PC1500Display disp = m_pc1500->display();
        frame.cols = PC1500Display::kCols;
        frame.rows = PC1500Display::kRows;
        frame.pixels.resize(static_cast<std::size_t>(frame.cols) * frame.rows);
        for (int row = 0; row < frame.rows; ++row) {
            for (int col = 0; col < frame.cols; ++col) {
                frame.pixels[static_cast<std::size_t>(row) * frame.cols + col] = disp.pixel(col, row);
            }
        }
        frame.poweredOn = m_pc1500->isDisplayOn();

        frame.statusSymbols = {
            {"BUSY", disp.busy()},       {"SHIFT", disp.shift()},
            {"JAPANESE", disp.japanese()}, {"SMALL", disp.small()},
            {"ROMAN_I", disp.romanI()},  {"ROMAN_II", disp.romanII()},
            {"ROMAN_III", disp.romanIII()}, {"DEF", disp.def()},
            {"DE", disp.de()},           {"G", disp.g()},
            {"RAD", disp.rad()},         {"RESERVE", disp.reserve()},
            {"PRO", disp.pro()},         {"RUN", disp.run()},
        };
    }
    return frame;
}

// ---- Debug panel support ----

std::uint8_t MachineController::debugPeek(std::uint16_t addr) const {
    return withMachine(std::uint8_t{0}, [&](auto& machine) { return machine.debugPeek(addr); });
}

bool MachineController::debugSlotResponds(std::uint16_t addr) const {
    return m_pc1500 ? m_pc1500->debugSlotResponds(addr) : false;
}

int MachineController::debugSlotCardBank() const {
    return m_pc1500 ? m_pc1500->debugSlotCardBank() : -1;
}

int MachineController::debugSlotCardBankCount() const {
    return m_pc1500 ? m_pc1500->debugSlotCardBankCount() : -1;
}

std::vector<std::uint8_t> MachineController::debugSlotCardImage() const {
    return m_pc1500 ? m_pc1500->debugSlotCardImage() : std::vector<std::uint8_t>{};
}

bool MachineController::slot1Attached() const {
    return m_pc1600 && m_pc1600->slot1Attached();
}

bool MachineController::slot2Attached() const {
    return m_pc1600 && m_pc1600->slot2Attached();
}

std::vector<std::uint8_t> MachineController::debugSlotImagePC1600(int slot) const {
    return m_pc1600 ? m_pc1600->debugSlotImage(slot) : std::vector<std::uint8_t>{};
}

std::vector<std::uint8_t> MachineController::debugInternalRamPC1600() const {
    if (!m_pc1600) return {};
    std::vector<std::uint8_t> out(PC1600Machine::kInternalRamSize);
    m_pc1600->debugCopyInternalRam(out.data());
    return out;
}

namespace {
DebugPageTarget toDebugPageTarget(PC1600Machine::PageTarget t) {
    switch (t) {
        case PC1600Machine::PageTarget::OpenBus:       return DebugPageTarget::OpenBus;
        case PC1600Machine::PageTarget::SystemRomLo:   return DebugPageTarget::SystemRomLo;
        case PC1600Machine::PageTarget::SystemRomHi:   return DebugPageTarget::SystemRomHi;
        case PC1600Machine::PageTarget::Bank3Rom:      return DebugPageTarget::Bank3Rom;
        case PC1600Machine::PageTarget::Bank3bRom:     return DebugPageTarget::Bank3bRom;
        case PC1600Machine::PageTarget::Bank6Rom:      return DebugPageTarget::Bank6Rom;
        case PC1600Machine::PageTarget::PeripheralRom: return DebugPageTarget::PeripheralRom;
        case PC1600Machine::PageTarget::InternalRam:   return DebugPageTarget::InternalRam;
        case PC1600Machine::PageTarget::Slot1:         return DebugPageTarget::Slot1;
        case PC1600Machine::PageTarget::Slot2:         return DebugPageTarget::Slot2;
    }
    return DebugPageTarget::OpenBus;
}
}  // namespace

DebugBankStateFrame MachineController::debugBankStatePC1600() const {
    DebugBankStateFrame out;
    if (!m_pc1600) return out;
    const PC1600Machine::DebugBankState bs = m_pc1600->debugBankState();
    out.port31 = bs.port31; out.port28 = bs.port28; out.port3c = bs.port3c;
    out.pageABank = bs.pageABank; out.pageBBank = bs.pageBBank;
    out.pageCBank = bs.pageCBank; out.pageDBank = bs.pageDBank;
    out.slot2MapMode = bs.slot2MapMode;
    out.slot1MapRemapped = bs.slot1MapRemapped;
    out.hiddenBasicRom = bs.hiddenBasicRom;
    out.slot1CardBank = bs.slot1CardBank; out.slot2CardBank = bs.slot2CardBank;
    out.slot1CardBankCount = bs.slot1CardBankCount; out.slot2CardBankCount = bs.slot2CardBankCount;
    for (int i = 0; i < 4; ++i) {
        out.target[i] = toDebugPageTarget(bs.target[i]);
        out.slotmapRedirect[i] = bs.slotmapRedirect[i];
    }
    return out;
}

bool MachineController::beginTrace(const QString& path) {
    return withMachine(false, [&](auto& machine) {
        std::FILE* handle = std::fopen(path.toStdString().c_str(), "wb");
        if (!handle) return false;
        const bool ok = machine.beginCpuTrace(handle, TRACE_FULL);
        if (!ok) std::fclose(handle); // a capture was already active; ownership passes only on success
        return ok;
    });
}

void MachineController::endTrace() {
    withMachine([](auto& machine) { machine.endCpuTrace(); });
}

std::uint64_t MachineController::traceBytes() const {
    return withMachine(std::uint64_t{0}, [](auto& machine) { return machine.cpuTraceBytes(); });
}

bool MachineController::traceActive() const {
    return withMachine(false, [](auto& machine) { return machine.cpuTraceActive(); });
}

void MachineController::discardMachine() {
    if (m_debug) m_debug->machineAboutToChange();
    endTrace();
    m_paste.cancel({}); // the machine it was typing into is going away
    m_pc1500.reset();
    m_pc1600.reset();
}


// ---- Plotter support ----

void MachineController::flushFloppyBeforeDetach() {
    if (m_floppyManager && ce1600pAttached()) m_floppyManager->flushPendingPersist();
}

bool MachineController::attachCE150(QString* error) {
    flushFloppyBeforeDetach(); // on a PC-1600, Core detaches the CE-1600P/F to make room
    std::string err;
    const bool ok = withMachine(false, [&](auto& machine) {
        return BundledRoms::attachCE150(machine, bundledRomDirs(), &err);
    });
    if (!ok && error) *error = QString::fromStdString(err);
    return ok;
}

void MachineController::detachCE150() {
    withMachine([](auto& machine) { machine.detachCE150(); });
}

bool MachineController::ce150Attached() const {
    return withMachine(false, [](auto& machine) { return machine.ce150Attached(); });
}

bool MachineController::attachCE158(QString* error) {
    flushFloppyBeforeDetach(); // on a PC-1600, Core detaches the CE-1600P/F to make room
    std::string err;
    const bool ok = withMachine(false, [&](auto& machine) {
        return BundledRoms::attachCE158(machine, bundledRomDirs(), &err);
    });
    if (!ok && error) *error = QString::fromStdString(err);
    return ok;
}

void MachineController::detachCE158() {
    withMachine([](auto& machine) { machine.detachCE158(); });
}

bool MachineController::ce158Attached() const {
    return withMachine(false, [](auto& machine) { return machine.ce158Attached(); });
}

std::vector<std::uint8_t> MachineController::drainCE158PrinterOutput() {
    return withMachine(std::vector<std::uint8_t>{}, [](auto& machine) { return machine.drainCE158ParallelOutput(); });
}

bool MachineController::attachCE1600P(QString* error) {
    if (!m_pc1600) return false; // (Core drops a CE-158 to make room -- PlotterController reports it)
    const auto versionName = [](CE1600PRomVersion v) { return v == CE1600PRomVersion::Old ? "old" : "new"; };
    std::string err;
    if (!BundledRoms::attachCE1600P(*m_pc1600, bundledRomDirs(), versionName(m_ce1600pRomVersion), &err)) {
        // Like the PC-1600's own old ROM: the old set is optional (its dump
        // may be missing), so warn and fall back to the new one. A failing
        // new set just leaves the plotter detached.
        if (m_ce1600pRomVersion != CE1600PRomVersion::Old) {
            if (error) *error = QString::fromStdString(err);
            return false;
        }
        QMessageBox::warning(nullptr, QObject::tr("Old CE-1600P ROM unavailable"),
                             QObject::tr("The old CE-1600P ROM could not be loaded:\n\n%1\n\n"
                                         "Using the new ROM instead.")
                                 .arg(QString::fromStdString(err)));
        m_ce1600pRomVersion = CE1600PRomVersion::New;
        err.clear();
        if (!BundledRoms::attachCE1600P(*m_pc1600, bundledRomDirs(), "new", &err)) {
            if (error) *error = QString::fromStdString(err);
            return false;
        }
    }
    if (m_floppyManager) m_floppyManager->insertSelectedDisk();
    return true;
}

void MachineController::detachCE1600P() {
    if (!m_pc1600) return;
    flushFloppyBeforeDetach();
    m_pc1600->detachCE1600P();
}

bool MachineController::ce1600pAttached() const {
    return m_pc1600 && m_pc1600->ce1600pAttached();
}

std::vector<AlpsPlotterMechanism::FlatPoint> MachineController::ce150PlotPoints() const {
    return withMachine(std::vector<AlpsPlotterMechanism::FlatPoint>{},
                       [](auto& machine) { return machine.ce150PlotPoints(); });
}

std::uint64_t MachineController::ce150PlotRevision() const {
    return withMachine(std::uint64_t{0}, [](auto& machine) { return machine.ce150PlotRevision(); });
}

void MachineController::clearCE150Paper() {
    withMachine([](auto& machine) { machine.clearCE150Paper(); });
}

std::vector<AlpsPlotterMechanism::FlatPoint> MachineController::ce1600pPlotPoints() const {
    return m_pc1600 ? m_pc1600->ce1600pPlotPoints() : std::vector<AlpsPlotterMechanism::FlatPoint>{};
}

std::uint64_t MachineController::ce1600pPlotRevision() const {
    return m_pc1600 ? m_pc1600->ce1600pPlotRevision() : 0;
}

void MachineController::clearCE1600PPaper() {
    if (m_pc1600) m_pc1600->clearCE1600PPaper();
}

bool MachineController::isMachinePoweredOn() const {
    if (m_pc1600) return !m_pc1600->isPoweredOff();
    if (m_pc1500) return !m_pc1500->cpu().poweredOff();
    return false;
}
