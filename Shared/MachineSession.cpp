#include "Shared/MachineSession.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

#include "Debug/Inspect/PC1500Inspector.hpp"
#include "Debug/Inspect/PC1600Inspector.hpp"
#include "HostClock.hpp"
#include "PC1500/PC1500BasicTyper.hpp"
#include "PC1500/PC1500LcdText.hpp"
#include "PC1500/PC1500Machine.hpp"
#include "PC1500/PC1500Screenshot.hpp"
#include "PC1500/PC1500TypedInput.hpp"
#include "PC1600/PC1600BasicTyper.hpp"
#include "PC1600/PC1600LcdText.hpp"
#include "PC1600/PC1600Machine.hpp"
#include "PC1600/PC1600Screenshot.hpp"
#include "PC1600/PC1600TypedInput.hpp"
#include "Resources/BundledRomCatalog.hpp"
#include "Shared/Utf8Path.hpp"

namespace shared {

namespace {

const char* pc1500RomVariantName(PC1500RomRevision revision) {
    switch (revision) {
        case PC1500RomRevision::A01: return "A01";
        case PC1500RomRevision::A03: return "A03";
        case PC1500RomRevision::A04: return "A04";
    }
    return "A04";
}

// ~4 frames @60Hz: long enough for the ROM's key-scan loop to see the press.
constexpr int kKeyHoldFrames = 4;
// 3 s of emulated time: don't hang if the power-down path never completes.
constexpr int kPowerOffTimeoutFrames = 180;

// tapShiftedKey(): 30 ms shift hold, 100 ms gap, 30 ms base-key hold.
constexpr double kTapHoldSeconds = 0.030;
constexpr double kTapGapSeconds = 0.100;

} // namespace

MachineSession::MachineSession(Settings& settings, const AppPaths& paths, Listener& listener)
    : m_settings(settings),
      m_paths(paths),
      m_listener(listener),
      m_ce158SerialLink([this] {
          return std::make_unique<PtySerialLink>(effectiveSerialLinkDir(), PtySerialLink::kCE158LinkName);
      }) {}

MachineSession::~MachineSession() = default;

void MachineSession::resetRomSelectionsToDefault() {
    m_pc1500RomRevision = PC1500RomRevision::A04;
    m_pc1600RomVersion = PC1600RomVersion::New;
    m_ce1600pRomVersion = CE1600PRomVersion::New;
}

void MachineSession::romMissing(const std::string& error) {
    m_listener.romMissing(error, m_paths.bundledResourcesDir());
    throw std::runtime_error(error);
}

void MachineSession::switchModel(Model model, bool keepPlotter) {
    job::runToCompletion(switchModelJob(model, keepPlotter));
}

job::Task<void> MachineSession::switchModelJob(Model model, bool keepPlotter) {
    const bool restoreCE150 = keepPlotter && ce150Attached();
    const bool restoreCE1600P = keepPlotter && ce1600pAttached();
    const bool restoreCE158 = keepPlotter && ce158Attached();
    const std::string restoreHostDrive = keepPlotter && model == Model::PC1600 ? hostDriveDirectory() : std::string();
    flushFloppyBeforeDetach(); // the old machine (and its disk) is going away
    discardMachine();
    m_model = model;

    if (model == Model::PC1600) {
        makePC1600WithRomFallback();
        wireNewMachine();

        // Attach any currently-selected memory modules before the cold
        // boot -- a module's state must be visible on the very first ROM
        // check.
        m_listener.attachModulesToFreshMachine();
        if (restoreCE150) attachCE150();
        if (restoreCE1600P) attachCE1600P();
        if (restoreCE158) attachCE158();
        if (!restoreHostDrive.empty()) attachHostDrive(restoreHostDrive);
    } else {
        const auto variant = (model == Model::PC1500A) ? PC1500Variant::PC1500A : PC1500Variant::PC1500;
        // PC-1500A is A04-only (PC1500Variant.hpp) -- clamp regardless of
        // whatever revision was last picked for the plain PC-1500.
        if (variant == PC1500Variant::PC1500A) m_pc1500RomRevision = PC1500RomRevision::A04;
        m_pc1500 = std::make_unique<PC1500Machine>(variant);
        wireNewMachine();

        std::string err;
        if (!BundledRoms::loadPC1500Rom(*m_pc1500, pc1500RomVariantName(m_pc1500RomRevision), bundledRomDirs(),
                                        &err)) {
            romMissing(err);
        }

        m_listener.attachModulesToFreshMachine();
        if (restoreCE150) attachCE150();
        if (restoreCE158) attachCE158();
    }

    // Every rebuild is a full cold boot, run flat out to the prompt (incl. a
    // plotter's power-on init); the caller restarts paced emulation after
    // it, which sets the clock from the host (see seedClockFromHost()).
    // PC1500Machine has only one reset level, so only the PC-1600 gets the
    // ALL RESET; a freshly-constructed machine's RAM is cleared either way.
    co_await resetToPromptJob(/*allReset=*/model == Model::PC1600);
    discardAudio(); // whatever the flat-out boot beeped is stale

    m_settings.setLastUsedModel(static_cast<int>(m_model));
    m_listener.modelSwitched(m_model);
}

void MachineSession::setPC1600RomVersion(PC1600RomVersion version) {
    job::runToCompletion(setPC1600RomVersionJob(version));
}

job::Task<void> MachineSession::setPC1600RomVersionJob(PC1600RomVersion version) {
    m_pc1600RomVersion = version;
    if (m_model == Model::PC1600) co_await switchModelJob(m_model, /*keepPlotter=*/true); // rebuild with the new ROM
}

void MachineSession::setPC1500RomRevision(PC1500RomRevision revision) {
    job::runToCompletion(setPC1500RomRevisionJob(revision));
}

job::Task<void> MachineSession::setPC1500RomRevisionJob(PC1500RomRevision revision) {
    m_pc1500RomRevision = revision;
    if (m_model != Model::PC1600) co_await switchModelJob(m_model, /*keepPlotter=*/true); // rebuild with the new ROM
}

std::vector<std::string> MachineSession::bundledRomDirs() const { return {m_paths.bundledResourcesDir()}; }

void MachineSession::makePC1600WithRomFallback() {
    std::string romErr;
    for (;;) {
        m_pc1600 = std::make_unique<PC1600Machine>();
        const char* version = m_pc1600RomVersion == PC1600RomVersion::Old ? "old" : "new";
        if (BundledRoms::loadPC1600RomSet(*m_pc1600, bundledRomDirs(), version, &romErr)) return;
        // Only the old ROM set is optional (its dump may be missing or
        // incomplete): fall back to the new ROM and retry once -- a new-ROM
        // failure ends the session, so the retry can't loop a third time.
        if (m_pc1600RomVersion != PC1600RomVersion::Old) romMissing(romErr);
        m_listener.notice(Notice::OldPC1600RomUnavailable, romErr);
        m_pc1600RomVersion = PC1600RomVersion::New;
    }
}

PC1500Machine& MachineSession::resetBareForPresetPC1500(PC1500Variant variant) {
    discardMachine();
    m_pc1500 = std::make_unique<PC1500Machine>(variant);
    wireNewMachine(); // a preset's `interface: CE-158` picks up the link from here
    return *m_pc1500;
}

PC1600Machine& MachineSession::resetBareForPresetPC1600(PC1600RomVersion version, CE1600PRomVersion ce1600pVersion) {
    m_pc1600RomVersion = version;
    m_ce1600pRomVersion = ce1600pVersion;
    discardMachine();
    makePC1600WithRomFallback();
    wireNewMachine(); // see resetBareForPresetPC1500
    return *m_pc1600;
}

void MachineSession::finishPresetLoad(Model model) {
    m_model = model;
    m_settings.setLastUsedModel(static_cast<int>(m_model));
    m_listener.modelSwitched(m_model);
}

std::string MachineSession::effectiveSerialLinkDir() const {
    const std::string dir = m_settings.serialLinkDirOverride();
    return dir.empty() ? m_paths.instanceDir() : dir;
}

void MachineSession::refreshSerialLinkDirectory() {
    const std::string dir = effectiveSerialLinkDir();
    if (m_serialLink) m_serialLink->relink(dir);
    if (PtySerialLink* ce158 = m_ce158SerialLink.link()) ce158->relink(dir);
    m_listener.serialLinksRelinked();
}

std::string MachineSession::serialLinkStatus() const {
    if (!m_serialLink || !m_serialLink->isOpen()) return {};
    return m_serialLink->preferredPath();
}

std::string MachineSession::ce158SerialLinkStatus() const {
    const PtySerialLink* link = m_ce158SerialLink.link();
    if (!link || !link->isOpen()) return {};
    return link->preferredPath();
}

void MachineSession::wireNewMachine() {
    withMachine([this](auto& machine) { machine.setCE158SerialLink(&m_ce158SerialLink); });
    if (m_pc1600) {
        if (!m_serialLink) m_serialLink = std::make_unique<PtySerialLink>(effectiveSerialLinkDir());
        m_pc1600->setSerialLink(m_serialLink.get());
    }
    m_listener.machineReplaced();
}

void MachineSession::discardMachine() {
    m_listener.machineAboutToChange();
    std::string tapeError;
    if (!tapeEject(&tapeError)) m_listener.notice(Notice::TapeSaveFailed, tapeError); // a recording goes to its file
    endTrace();
    m_paste.cancel({}); // the machine it was typing into is going away
    m_liveTyping = false;
    m_timedKeys.clear();
    m_pc1500.reset();
    m_pc1600.reset();
}

void MachineSession::seedClockFromHost() {
    withMachine([](auto& machine) { seedClockFromHostTime(machine); });
}

void MachineSession::resetToPrompt(bool allReset) { job::runToCompletion(resetToPromptJob(allReset)); }

job::Task<void> MachineSession::resetToPromptJob(bool allReset) {
    cancelPaste();
    if (m_pc1600) {
        if (allReset) m_pc1600->allReset();
        else m_pc1600->reset();
        co_await runBootToPromptJob(*m_pc1600);
    } else if (m_pc1500) {
        if (allReset) m_pc1500->allReset();
        else m_pc1500->reset();
        co_await runBootToPromptJob(*m_pc1500);
    }
}

namespace {

// The OFF / power-down / change / ON / boot cycle on one machine type.
template <class Machine>
job::Task<void> powerCycle(Machine& machine, std::uint64_t frame, std::function<void()> change,
                           std::function<bool()> poweredOn) {
    machine.pressKey("off");
    co_await job::run(machine, frame * kKeyHoldFrames);
    machine.releaseKey("off");
    for (int i = 0; i < kPowerOffTimeoutFrames && poweredOn(); ++i) co_await job::run(machine, frame);
    change();
    machine.setOnKeyPressed(true);
    co_await job::run(machine, frame * kKeyHoldFrames);
    machine.setOnKeyPressed(false);
    co_await runBootToPromptJob(machine);
}

} // namespace

void MachineSession::powerCycleAround(const std::function<void()>& change) {
    job::runToCompletion(powerCycleAroundJob(change));
}

job::Task<void> MachineSession::powerCycleAroundJob(std::function<void()> change) {
    if (!isMachinePoweredOn()) {
        change();  // already off: no synthetic power cycle needed
        co_return;
    }
    cancelPaste();
    const auto frame = static_cast<std::uint64_t>(clockHz() / 60);
    const auto poweredOn = [this] { return isMachinePoweredOn(); };
    if (m_pc1600) co_await powerCycle(*m_pc1600, frame, change, poweredOn);
    else if (m_pc1500) co_await powerCycle(*m_pc1500, frame, change, poweredOn);
}

// ---- Keys and typing ----

void MachineSession::pressKey(const std::string& name) {
    withMachine([&](auto& machine) { machine.pressKey(name); });
}

void MachineSession::releaseKey(const std::string& name) {
    withMachine([&](auto& machine) { machine.releaseKey(name); });
}

void MachineSession::setOnKeyPressed(bool pressed) {
    withMachine([&](auto& machine) { machine.setOnKeyPressed(pressed); });
}

void MachineSession::scheduleKey(double afterSeconds, bool press, const std::string& name) {
    const auto due = m_budgetClock + static_cast<std::uint64_t>(afterSeconds * clockHz());
    // Keep the queue in due order (taps can overlap).
    auto it = std::upper_bound(m_timedKeys.begin(), m_timedKeys.end(), due,
                               [](std::uint64_t d, const TimedKey& k) { return d < k.due; });
    m_timedKeys.insert(it, TimedKey{due, press, name});
}

void MachineSession::fireDueKeys() {
    while (!m_timedKeys.empty() && m_timedKeys.front().due <= m_budgetClock) {
        const TimedKey key = m_timedKeys.front();
        m_timedKeys.pop_front();
        if (key.press) pressKey(key.name);
        else releaseKey(key.name);
    }
}

void MachineSession::tapShiftedKey(const std::string& baseName) {
    pressKey("shift");
    scheduleKey(kTapHoldSeconds, false, "shift");
    scheduleKey(kTapHoldSeconds + kTapGapSeconds, true, baseName);
    scheduleKey(kTapHoldSeconds + kTapGapSeconds + kTapHoldSeconds, false, baseName);
}

void MachineSession::tapKey(const std::string& name) {
    if (m_pc1500) {
        m_pc1500->enqueueKey(name);
    } else if (m_pc1600) {
        pressKey(name);
        scheduleKey(kTapHoldSeconds, false, name);
    }
}

void MachineSession::enqueueKey(const std::string& name) {
    // PC-1600 buffering is out of scope for now -- silently ignored,
    // matching pressKey()/releaseKey()'s convention for a name/model
    // combination that doesn't apply.
    if (m_pc1500) m_pc1500->enqueueKey(name);
}

void MachineSession::enqueueShiftedKey(const std::string& baseName) {
    enqueueKey("shift");
    enqueueKey(baseName);
}

void MachineSession::pasteText(const std::string& text) { enqueueTyped(text, /*pressEnter=*/false); }

void MachineSession::typeCommand(const std::string& line) { enqueueTyped(line, /*pressEnter=*/true); }

void MachineSession::enqueueTyped(const std::string& text, bool pressEnter) {
    if (!m_pc1500 && !m_pc1600) return;
    if (!m_paste.active()) m_pasteFrameCycles = 0;
    std::vector<PasteStep> steps = m_pc1600 ? buildPasteSteps(text, pc1600ResolveTypedChar, pc1600ResolveKbiiChar)
                                            : buildPasteSteps(text, pc1500ResolveTypedChar);
    if (pressEnter) steps.push_back(PasteStep{"enter", false});
    m_paste.setPacing(m_pc1600 ? pc1600PastePacing() : pc1500PastePacing());
    m_paste.append(steps);
    m_liveTyping = false;
}

void MachineSession::typeLiveStep(const PasteStep& step) {
    if (!m_pc1600) return;
    if (!m_paste.active()) m_pasteFrameCycles = 0;
    m_paste.setPacing(pc1600PastePacing());
    m_paste.append({step});
    m_liveTyping = true;
}

void MachineSession::cancelPaste() {
    m_paste.cancel([this](const std::string& key) { releaseKey(key); });
    m_liveTyping = false;
}

void MachineSession::interruptPaste() {
    m_paste.cancel([this](const std::string& key) { releaseKey(key); }, /*finishKbii=*/true);
    m_liveTyping = m_paste.active(); // what's left is a KBII sequence's closing tap
}

// ---- Running ----

void MachineSession::runActive(std::uint64_t cycles) {
    if (m_listener.runDebuggerSlice(cycles)) return;
    withMachine([&](auto& machine) { machine.runCycles(cycles); });
}

void MachineSession::pasteOnFrame() {
    m_paste.onFrame([this](const std::string& key) { pressKey(key); },
                    [this](const std::string& key) { releaseKey(key); },
                    [this] { return m_pc1600 ? pc1600ReadLatches(*m_pc1600) : KeyLatches{}; });
}

void MachineSession::advance(std::uint64_t cyclesBudget) {
    m_budgetClock += cyclesBudget;
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
    fireDueKeys();
}

double MachineSession::clockHz() const {
    if (m_pc1600) return static_cast<double>(PC1600Machine::kTStateHz);
    return static_cast<double>(PC1500Machine::kCpuHz);
}

bool MachineSession::isMachinePoweredOn() const {
    if (m_pc1600) return !m_pc1600->isPoweredOff();
    if (m_pc1500) return !m_pc1500->cpu().poweredOff();
    return false;
}

// ---- Output ----

DisplayFrame MachineSession::currentDisplay() const {
    DisplayFrame frame;
    auto fill = [&frame](LcdBitmap bitmap, const StatusLine& line) {
        frame.cols = bitmap.cols;
        frame.rows = bitmap.rows;
        frame.pixels = std::move(bitmap.pixels);
        frame.poweredOn = bitmap.poweredOn;
        for (std::size_t i = 0; i < StatusLine::kCount; ++i)
            frame.statusSymbols.emplace_back(kStatusSymbolNames[i], line.all()[i]);
    };
    if (m_pc1600) {
        const PC1600DisplaySnapshot snap = m_pc1600->displaySnapshot();
        fill(pc1600LcdBitmap(snap), snap.statusLine);
    } else if (m_pc1500) {
        const PC1500Display disp = m_pc1500->display();
        fill(pc1500LcdBitmap(disp, m_pc1500->isDisplayOn()), disp.statusLine());
    }
    return frame;
}

bool MachineSession::lcdText(LcdText* out) const {
    if (m_pc1600) *out = pc1600LcdText(*m_pc1600);
    else if (m_pc1500) *out = pc1500LcdText(*m_pc1500);
    else return false;
    return true;
}

GrayImage MachineSession::currentScreenImage() const {
    if (m_pc1600) return renderLcdImage(pc1600LcdBitmap(*m_pc1600), kPC1600ScreenMm);
    if (m_pc1500) return renderLcdImage(pc1500LcdBitmap(*m_pc1500), kPC1500ScreenMm);
    return GrayImage{};
}

std::size_t MachineSession::drainAudio(std::int16_t* out, std::size_t max) {
    return withMachine(std::size_t{0}, [&](auto& machine) { return machine.drainAudio(out, max); });
}

void MachineSession::discardAudio() {
    withMachine([](auto& machine) { machine.discardAudio(); });
}

// ---- Debug panel ----

std::vector<std::string> MachineSession::inspectorView(inspect::View view) const {
    if (m_pc1600) return m_pc1600->debugInspect([&](const PC1600Machine& m) { return inspect::pc1600View(m, view); });
    if (m_pc1500) return m_pc1500->debugInspect([&](const PC1500Machine& m) { return inspect::pc1500View(m, view); });
    return {};
}

std::vector<inspect::MenuEntry> MachineSession::inspectorMenu(bool dumps) const {
    if (m_pc1600) return m_pc1600->debugInspect([&](const PC1600Machine& m) { return inspect::pc1600Menu(m, dumps); });
    if (m_pc1500) return m_pc1500->debugInspect([&](const PC1500Machine& m) { return inspect::pc1500Menu(m, dumps); });
    return {};
}

std::vector<std::uint8_t> MachineSession::debugSlotCardImage(int slot, int* bankCount) const {
    auto read = [&](const ExpansionCard* card) {
        *bankCount = card ? card->debugBankCount() : -1;
        return card ? card->debugImage() : std::vector<std::uint8_t>{};
    };
    if (m_pc1600)
        return m_pc1600->debugInspect([&](const PC1600Machine& m) { return read(m.memory().slotCard(slot)); });
    if (m_pc1500)
        return m_pc1500->debugInspect(
            [&](const PC1500Machine& m) { return read(m.memory().expansionConnector().attachedCard()); });
    *bankCount = -1;
    return {};
}

bool MachineSession::beginTrace(const std::string& path) {
    return withMachine(false, [&](auto& machine) {
        std::FILE* handle = std::fopen(toPath(path).string().c_str(), "wb");
        if (!handle) return false;
        const bool ok = machine.beginCpuTrace(handle, TRACE_FULL);
        if (!ok) std::fclose(handle); // a capture was already active; ownership passes only on success
        return ok;
    });
}

void MachineSession::endTrace() {
    withMachine([](auto& machine) { machine.endCpuTrace(); });
}

std::uint64_t MachineSession::traceBytes() const {
    return withMachine(std::uint64_t{0}, [](auto& machine) { return machine.cpuTraceBytes(); });
}

bool MachineSession::traceActive() const {
    return withMachine(false, [](auto& machine) { return machine.cpuTraceActive(); });
}

// ---- Cassette recorder ----

bool MachineSession::tapeInterfaceAttached() const {
    if (m_pc1600) return m_pc1600->ce1600pAttached();
    return m_pc1500 && m_pc1500->ce150Attached();
}

bool MachineSession::tapePlay(const std::string& path, std::string* error) {
    std::string err;
    const bool ok = withMachine(false, [&](auto& machine) { return machine.tapePlay(path, err); });
    if (!ok && error) *error = err;
    return ok;
}

void MachineSession::tapeRecord(const std::string& path) {
    withMachine([&](auto& machine) { machine.tapeRecord(path); });
}

bool MachineSession::tapeEject(std::string* error) {
    std::string err;
    const bool ok = withMachine(true, [&](auto& machine) { return machine.tapeEject(&err); });
    if (!ok && error) *error = err;
    return ok;
}

TapeDeck::Status MachineSession::tapeStatus() const {
    return withMachine(TapeDeck::Status{}, [](auto& machine) { return machine.tapeStatus(); });
}

std::string MachineSession::tapeTakeLastError() {
    return withMachine(std::string(), [](auto& machine) { return machine.tapeTakeLastError(); });
}

// ---- Peripherals ----

void MachineSession::flushFloppyBeforeDetach() {
    if (ce1600pAttached()) m_listener.flushFloppy();
}

bool MachineSession::attachCE150(std::string* error) {
    flushFloppyBeforeDetach(); // on a PC-1600, Core detaches the CE-1600P/F to make room
    std::string err;
    const bool ok =
        withMachine(false, [&](auto& machine) { return BundledRoms::attachCE150(machine, bundledRomDirs(), &err); });
    if (!ok && error) *error = err;
    return ok;
}

void MachineSession::detachCE150() {
    withMachine([](auto& machine) { machine.detachCE150(); });
}

bool MachineSession::ce150Attached() const {
    return withMachine(false, [](auto& machine) { return machine.ce150Attached(); });
}

bool MachineSession::attachCE158(std::string* error) {
    flushFloppyBeforeDetach(); // on a PC-1600, Core detaches the CE-1600P/F to make room
    std::string err;
    const bool ok =
        withMachine(false, [&](auto& machine) { return BundledRoms::attachCE158(machine, bundledRomDirs(), &err); });
    if (!ok && error) *error = err;
    return ok;
}

void MachineSession::detachCE158() {
    withMachine([](auto& machine) { machine.detachCE158(); });
}

bool MachineSession::ce158Attached() const {
    return withMachine(false, [](auto& machine) { return machine.ce158Attached(); });
}

std::vector<std::uint8_t> MachineSession::drainCE158PrinterOutput() {
    return withMachine(std::vector<std::uint8_t>{}, [](auto& machine) { return machine.drainCE158ParallelOutput(); });
}

bool MachineSession::attachCE1600P(std::string* error) {
    if (!m_pc1600) return false; // (Core drops a CE-158 to make room)
    const auto versionName = [](CE1600PRomVersion v) { return v == CE1600PRomVersion::Old ? "old" : "new"; };
    std::string err;
    if (!BundledRoms::attachCE1600P(*m_pc1600, bundledRomDirs(), versionName(m_ce1600pRomVersion), &err)) {
        // Like the PC-1600's own old ROM: the old set is optional (its dump
        // may be missing), so fall back to the new one. A failing new set
        // just leaves the plotter detached.
        if (m_ce1600pRomVersion != CE1600PRomVersion::Old) {
            if (error) *error = err;
            return false;
        }
        m_listener.notice(Notice::OldCE1600PRomUnavailable, err);
        m_ce1600pRomVersion = CE1600PRomVersion::New;
        err.clear();
        if (!BundledRoms::attachCE1600P(*m_pc1600, bundledRomDirs(), "new", &err)) {
            if (error) *error = err;
            return false;
        }
    }
    m_listener.insertSelectedDisk();
    return true;
}

void MachineSession::detachCE1600P() {
    if (!m_pc1600) return;
    flushFloppyBeforeDetach();
    m_pc1600->detachCE1600P();
}

bool MachineSession::swapCE1600PRom(CE1600PRomVersion version, std::string* error) {
    const int side = m_pc1600 ? m_pc1600->ce1600fSide() : 0;
    detachCE1600P();
    setCE1600PRomVersion(version);
    if (!attachCE1600P(error)) return false; // re-inserts the selected disk, side A up
    if (side != 0 && m_pc1600->ce1600fHasDisk()) m_pc1600->ce1600fSetSide(side);
    return true;
}

bool MachineSession::ce1600pAttached() const { return m_pc1600 && m_pc1600->ce1600pAttached(); }

bool MachineSession::attachHostDrive(const std::string& dir, std::string* error) {
    if (!m_pc1600) return false;
    const std::filesystem::path path = toPath(dir);
    if (m_pc1600->hostDriveAttached()) {
        m_pc1600->setHostDriveDirectory(path);
    } else {
        std::string err;
        if (!BundledRoms::attachHostDrive(*m_pc1600, bundledRomDirs(), path, &err)) {
            if (error) *error = err;
            return false;
        }
    }
    return true;
}

std::string MachineSession::hostDriveDirectory() const {
    if (!m_pc1600) return {};
    const std::u8string dir = m_pc1600->hostDriveDirectory().u8string();
    return std::string(dir.begin(), dir.end());
}

void MachineSession::detachHostDrive() {
    if (m_pc1600) m_pc1600->detachHostDrive();
}

bool MachineSession::hostDriveAttached() const { return m_pc1600 && m_pc1600->hostDriveAttached(); }

std::vector<AlpsPlotterMechanism::FlatPoint> MachineSession::ce150PlotPoints() const {
    return withMachine(std::vector<AlpsPlotterMechanism::FlatPoint>{},
                       [](auto& machine) { return machine.ce150PlotPoints(); });
}

std::uint64_t MachineSession::ce150PlotRevision() const {
    return withMachine(std::uint64_t{0}, [](auto& machine) { return machine.ce150PlotRevision(); });
}

void MachineSession::clearCE150Paper() {
    withMachine([](auto& machine) { machine.clearCE150Paper(); });
}

std::vector<AlpsPlotterMechanism::FlatPoint> MachineSession::ce1600pPlotPoints() const {
    return m_pc1600 ? m_pc1600->ce1600pPlotPoints() : std::vector<AlpsPlotterMechanism::FlatPoint>{};
}

std::uint64_t MachineSession::ce1600pPlotRevision() const { return m_pc1600 ? m_pc1600->ce1600pPlotRevision() : 0; }

void MachineSession::clearCE1600PPaper() {
    if (m_pc1600) m_pc1600->clearCE1600PPaper();
}

} // namespace shared
