#include "MachineController.hpp"

#include <cstdlib>

#include <QDateTime>
#include <QDebug>
#include <QMessageBox>
#include <QTimer>

#include "PC1500/PC1500Machine.hpp"
#include "PC1600/PC1600Machine.hpp"
#include "PC1600/PtySerialLink.hpp"
#include "Resources/BundledRomCatalog.hpp"
#include "AppPaths.hpp"
#include "AppSettings.hpp"
#include "FloppyDiskManager.hpp"
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
    switchModel(initialModel());
}

MachineController::~MachineController() = default;

void MachineController::switchModel(Model model) {
    m_model = model;
    m_pc1500.reset();
    m_pc1600.reset();

    if (model == Model::PC1600) {
        m_pc1600 = std::make_unique<PC1600Machine>();
        loadPC1600RomSet(*m_pc1600);

        // Attach any currently-selected memory modules before the cold
        // boot -- a module's state must be visible on the very first ROM
        // check.
        if (m_moduleManager) m_moduleManager->attachAllToFreshMachine();

        attachSerialLink(*m_pc1600);

        // App launch always does a full cold boot.
        m_pc1600->allReset();
        seedClockFromHost();
    } else {
        const auto variant = (model == Model::PC1500A) ? PC1500Variant::PC1500A : PC1500Variant::PC1500;
        // PC-1500A is A04-only (PC1500Variant.hpp) -- clamp regardless of
        // whatever revision was last picked for the plain PC-1500.
        if (variant == PC1500Variant::PC1500A) m_pc1500RomRevision = PC1500RomRevision::A04;
        m_pc1500 = std::make_unique<PC1500Machine>(variant);

        std::string err;
        if (!BundledRoms::loadPC1500Rom(*m_pc1500, pc1500RomVariantName(m_pc1500RomRevision),
                                        bundledRomDirs(), &err)) {
            reportMissingRomAndExit(err);
        }

        if (m_moduleManager) m_moduleManager->attachAllToFreshMachine();

        // PC1500Machine has only one reset level.
        m_pc1500->reset();
        seedClockFromHost();
    }

    AppSettings::setLastUsedModel(static_cast<int>(m_model));
    emit modelChanged(m_model);
}

void MachineController::setPC1500RomRevision(PC1500RomRevision revision) {
    m_pc1500RomRevision = revision;
    if (m_model != Model::PC1600) switchModel(m_model); // rebuild with the new ROM
}

std::vector<std::string> MachineController::bundledRomDirs() {
    return {AppPaths::bundledResourcesDir().toStdString()};
}

void MachineController::loadPC1600RomSet(PC1600Machine& machine) {
    std::string err;
    if (!BundledRoms::loadPC1600RomSet(machine, bundledRomDirs(), &err)) {
        reportMissingRomAndExit(err);
    }
}

PC1500Machine& MachineController::resetBareForPresetPC1500(PC1500Variant variant) {
    m_pc1500.reset();
    m_pc1600.reset();
    m_pc1500 = std::make_unique<PC1500Machine>(variant);
    return *m_pc1500;
}

PC1600Machine& MachineController::resetBareForPresetPC1600() {
    m_pc1500.reset();
    m_pc1600.reset();
    m_pc1600 = std::make_unique<PC1600Machine>();
    loadPC1600RomSet(*m_pc1600);
    attachSerialLink(*m_pc1600);
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
    if (!m_serialLink) return;
    m_serialLink->relink(effectiveSerialLinkDir().toStdString());
}

QString MachineController::serialLinkStatus() const {
    if (!m_serialLink || !m_serialLink->isOpen()) return QString();
    return QString::fromStdString(m_serialLink->preferredPath());
}

void MachineController::finishPresetLoad(Model model) {
    m_model = model;
    seedClockFromHost();
    AppSettings::setLastUsedModel(static_cast<int>(m_model));
    emit modelChanged(m_model);
}

void MachineController::seedClockFromHost() {
    const QDateTime now = QDateTime::currentDateTime();
    const int year = now.date().year();
    const int month = now.date().month();
    const int day = now.date().day();
    const int hour = now.time().hour();
    const int minute = now.time().minute();
    const int second = now.time().second();
    if (m_pc1600) {
        m_pc1600->seedClock(year, month, day, hour, minute, second);
    } else if (m_pc1500) {
        m_pc1500->seedClock(year, month, day, hour, minute, second);
    }
}

void MachineController::resetSimple() {
    if (m_pc1600) {
        m_pc1600->reset();
    } else if (m_pc1500) {
        m_pc1500->reset();
    }
}

void MachineController::resetAll() {
    if (m_pc1600) {
        m_pc1600->allReset();
        seedClockFromHost();
    } else if (m_pc1500) {
        // No distinct ALL RESET level exists for PC1500/1500A in Core.
        m_pc1500->reset();
    }
}

void MachineController::pressKey(const std::string& name) {
    if (m_pc1600) {
        m_pc1600->pressKey(name);
    } else if (m_pc1500) {
        m_pc1500->pressKey(name);
    }
}

void MachineController::releaseKey(const std::string& name) {
    if (m_pc1600) {
        m_pc1600->releaseKey(name);
    } else if (m_pc1500) {
        m_pc1500->releaseKey(name);
    }
}

void MachineController::setOnKeyPressed(bool pressed) {
    if (m_pc1600) {
        m_pc1600->setOnKeyPressed(pressed);
    } else if (m_pc1500) {
        m_pc1500->setOnKeyPressed(pressed);
    }
}

void MachineController::tapShiftedKey(const std::string& baseName) {
    // 30ms shift hold, 100ms gap between taps, 30ms base-key hold.
    pressKey("shift");
    QTimer::singleShot(30, this, [this] { releaseKey("shift"); });
    QTimer::singleShot(30 + 100, this, [this, baseName] { pressKey(baseName); });
    QTimer::singleShot(30 + 100 + 30, this, [this, baseName] { releaseKey(baseName); });
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

void MachineController::advance(std::uint64_t cyclesBudget) {
    if (m_pc1600) {
        m_pc1600->runCycles(cyclesBudget);
    } else if (m_pc1500) {
        m_pc1500->runCycles(cyclesBudget);
    }
}

double MachineController::clockHz() const {
    if (m_pc1600) {
        return static_cast<double>(PC1600Machine::kTStateHz);
    }
    // Upd1990ac::kCpuHz -- the LH5801's own clock, 1.3 MHz.
    return 1300000.0;
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
    if (m_pc1600) return m_pc1600->debugPeek(addr);
    if (m_pc1500) return m_pc1500->debugPeek(addr);
    return 0;
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

void MachineController::setTraceEnabled(bool enabled) {
    if (m_pc1600) {
        m_pc1600->setTraceEnabled(enabled);
        return;
    }
    if (!m_pc1500) return;
    const uint32_t flags = enabled ? (TRACE_PC | TRACE_REGS_LIGHT | TRACE_REGS_FULL) : TRACE_NONE;
    if (m_pc1500->traceFlags() != flags) m_pc1500->setTraceFlags(flags);
}

bool MachineController::traceEnabled() const {
    if (m_pc1600) return m_pc1600->traceEnabled();
    if (m_pc1500) return m_pc1500->traceFlags() != TRACE_NONE;
    return false;
}

std::uint32_t MachineController::drainPC1500Trace(CpuFrame* out, std::uint32_t max, std::uint32_t* outLost) {
    return m_pc1500 ? m_pc1500->drainTraceEvents(out, max, outLost) : 0;
}

std::uint32_t MachineController::drainSC7852Trace(Z80CpuFrame* out, std::uint32_t max, std::uint32_t* outLost) {
    return m_pc1600 ? m_pc1600->sc7852().drainTraceEvents(out, max, outLost) : 0;
}

std::uint32_t MachineController::drainLH5803Trace(CpuFrame* out, std::uint32_t max, std::uint32_t* outLost) {
    return m_pc1600 ? m_pc1600->lh5803().drainTraceEvents(out, max, outLost) : 0;
}

// ---- Plotter support ----

void MachineController::flushFloppyBeforeDetach() {
    if (m_floppyManager && ce1600pAttached()) m_floppyManager->flushPendingPersist();
}

bool MachineController::attachCE150() {
    std::string err;
    if (m_pc1600) {
        flushFloppyBeforeDetach();  // Core detaches the CE-1600P/F to make room
        return BundledRoms::attachCE150(*m_pc1600, bundledRomDirs(), &err);
    }
    if (m_pc1500) return BundledRoms::attachCE150(*m_pc1500, bundledRomDirs(), &err);
    return false;
}

void MachineController::detachCE150() {
    if (m_pc1600) m_pc1600->detachCE150();
    else if (m_pc1500) m_pc1500->detachCE150();
}

bool MachineController::ce150Attached() const {
    if (m_pc1600) return m_pc1600->ce150Attached();
    if (m_pc1500) return m_pc1500->ce150Attached();
    return false;
}

bool MachineController::attachCE1600P() {
    if (!m_pc1600) return false;
    std::string err;
    if (!BundledRoms::attachCE1600P(*m_pc1600, bundledRomDirs(), &err)) return false;
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
    if (m_pc1600) return m_pc1600->ce150PlotPoints();
    if (m_pc1500) return m_pc1500->ce150PlotPoints();
    return {};
}

std::uint64_t MachineController::ce150PlotRevision() const {
    if (m_pc1600) return m_pc1600->ce150PlotRevision();
    if (m_pc1500) return m_pc1500->ce150PlotRevision();
    return 0;
}

void MachineController::clearCE150Paper() {
    if (m_pc1600) m_pc1600->clearCE150Paper();
    else if (m_pc1500) m_pc1500->clearCE150Paper();
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
    if (m_pc1600) return m_pc1600->sc7852Owns();
    if (m_pc1500) return !m_pc1500->cpu().poweredOff();
    return false;
}
