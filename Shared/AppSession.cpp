#include "Shared/AppSession.hpp"

#include <chrono>
#include <utility>

#include "HostClock.hpp"
#include "PC1500/PC1500BasicLoader.hpp"
#include "PC1500/PC1500Machine.hpp"
#include "PC1500/PC1500MachineCodeLoader.hpp"
#include "PC1500/PC1500PresetLoader.hpp"
#include "PC1600/PC1600BasicLoader.hpp"
#include "PC1600/PC1600Machine.hpp"
#include "PC1600/PC1600MachineCodeLoader.hpp"
#include "PC1600/PC1600PresetLoader.hpp"
#include "Preset/PresetFile.hpp"

namespace shared {

MachineSession::NoticeText AppSession::attachFailedText(const std::string& device, const std::string& reason) {
    return {"Attach " + device, reason.empty() ? "The " + device + " could not be attached."
                                               : "The " + device + " could not be attached:\n\n" + reason};
}

namespace {

Model modelForPreset(const PresetFile& preset) {
    if (preset.isPC1600()) return Model::PC1600;
    return preset.variant == PC1500Variant::PC1500A ? Model::PC1500A : Model::PC1500;
}

const char* peripheralName(Peripheral p) {
    switch (p) {
        case Peripheral::CE150: return "CE-150";
        case Peripheral::CE1600P: return "CE-1600P";
        case Peripheral::CE158: return "CE-158";
    }
    return "";
}

} // namespace

AppSession::AppSession(Settings& settings, AppPaths& paths, Platform& platform)
    : m_settings(settings), m_paths(paths), m_platform(platform) {
    MachineSession::Bays& bays = *this;
    m_machine = std::make_unique<MachineSession>(m_settings, m_paths, m_platform, &bays);
    ModuleBay::Events moduleEvents;
    moduleEvents.moduleChanged = [this](int slot) { m_platform.moduleChanged(slot); };
    moduleEvents.error = [this](const std::string& text) { m_platform.bayError(Bay::Modules, text); };
    m_modules = std::make_unique<ModuleBay>(*m_machine, m_paths, std::move(moduleEvents));
    FloppyBay::Events floppyEvents;
    floppyEvents.error = [this](const std::string& text) { m_platform.bayError(Bay::Floppy, text); };
    m_floppy = std::make_unique<FloppyBay>(*m_machine, m_paths, std::move(floppyEvents));
    TapeBay::Events tapeEvents;
    tapeEvents.tapesChanged = [this] { m_platform.tapesChanged(); };
    tapeEvents.error = [this](const std::string& text) { m_platform.bayError(Bay::Tape, text); };
    m_tapes = std::make_unique<TapeBay>(*m_machine, m_paths, std::move(tapeEvents));
}

AppSession::~AppSession() {
    // Save what's pending; the machine goes last (the bays refer to it).
    if (m_modules) m_modules->flushPendingPersist();
    if (m_floppy) m_floppy->flushPendingPersist();
    m_tapes.reset();
    m_floppy.reset();
    m_modules.reset();
    m_machine.reset();
}

void AppSession::start() {
    m_machine->switchModel(m_settings.startupModel());
    m_machine->seedClockFromHost();
    m_pacer.restart();
}

double AppSession::nowSeconds() const { return static_cast<double>(Pacer::steadyNowNs()) * 1e-9; }

// ---- Running operations ----

void AppSession::beginOperation() {
    m_busy = true;
    m_platform.busyChanged(true);
    m_modules->flushPendingPersist();
    m_floppy->flushPendingPersist();
}

void AppSession::endOperation() {
    finishUnpaced();  // the bays catch up with whatever the operation changed
    m_machine->seedClockFromHost();
    m_pacer.restart();
    m_busy = false;
    m_platform.busyChanged(false);
}

bool AppSession::start(job::Task<OpResult> op, Done done) {
    if (m_busy) return false;
    beginOperation();
    m_op = job::Job<OpResult>(std::move(op));
    m_opDone = std::move(done);
    return true;
}

bool AppSession::stepSlice(job::Job<OpResult>& j) {
    const std::int64_t deadline = Pacer::steadyNowNs() + static_cast<std::int64_t>(kOperationSliceSeconds * 1e9);
    const auto cyclesPerStep = static_cast<std::uint64_t>(m_machine->clockHz() / 1000);
    for (;;) {
        if (j.step(cyclesPerStep)) return true;
        if (Pacer::steadyNowNs() >= deadline) return false;
    }
}

OpResult AppSession::takeResult(job::Job<OpResult>& j) {
    try {
        return j.result();
    } catch (const std::exception& e) {
        return OpResult::fail(e.what());
    }
}

OpResult AppSession::runNow(job::Task<OpResult> op, const std::function<void()>& pump) {
    if (m_busy) return job::runToCompletion(std::move(op));  // nested: straight through
    beginOperation();
    job::Job<OpResult> j(std::move(op));
    OpResult result;
    try {
        while (!stepSlice(j)) {
            if (pump) pump();
        }
        result = takeResult(j);
    } catch (const std::exception& e) {
        result = OpResult::fail(e.what());
    }
    endOperation();
    return result;
}

void AppSession::tick() {
    bool realTime = false;
    if (m_busy && m_op.valid()) {
        // A slice of the running operation instead of paced emulation.
        bool done = false;
        OpResult result;
        try {
            done = stepSlice(m_op);
            if (done) result = takeResult(m_op);
        } catch (const std::exception& e) {
            done = true;
            result = OpResult::fail(e.what());
        }
        if (done) {
            m_op = job::Job<OpResult>();
            Done doneFn = std::exchange(m_opDone, {});
            endOperation();
            if (doneFn) doneFn(result);
        }
    } else if (!m_busy) {
        realTime = !m_pacer.tick(*m_machine);
    }
    frameDuties();
    if (m_audio) m_audio->pump(*m_machine, /*discard=*/!realTime);
    else m_machine->discardAudio();
}

void AppSession::runFor(double seconds) {
    runEmulatedSeconds(*m_machine, seconds);
    finishUnpaced();
}

bool AppSession::runUntilPasteDone(double capSeconds) {
    const bool done = shared::runUntilPasteDone(*m_machine, capSeconds);
    finishUnpaced();
    return done;
}

void AppSession::attachAudio(AudioSink& sink, AudioPump::Warn warn) {
    m_audio = std::make_unique<AudioPump>(sink, std::move(warn));
}

void AppSession::warmUpAudio() {
    if (m_audio) m_audio->warmUp();
}

void AppSession::frameDuties() {
    const double now = nowSeconds();
    m_modules->tick(now);
    m_floppy->tick(now);
    m_tapeStatus = m_machine->tapeStatus();
    m_tapes->onFrameTick(m_tapeStatus);
}

void AppSession::finishUnpaced() {
    frameDuties();
    m_machine->discardAudio();
}

// ---- Operations ----

job::Task<OpResult> AppSession::loadPresetJob(std::string path, bool armOnly) {
    PresetFile preset;
    std::string parseError;
    if (!parsePresetFile(path, &preset, &parseError)) co_return OpResult::fail(parseError);
    preset.armOnly = armOnly;

    // Bundled catalog first, then the save folder -- the same order the
    // module bay uses, so a preset's `slot-N: <name>` resolves like the
    // picker. The bundled folder also holds the ROMs BundledRomCatalog
    // finds by name.
    const std::string bundled = m_paths.bundledResourcesDir();
    const std::vector<std::string> romDirs = {bundled};
    const std::vector<std::string> extraModuleDirs = {m_paths.instanceDir()};
    // `- trace:` and `- screenshot:` write into the trace folder.
    const std::string traceDir = m_paths.traceDir();
    const PresetLogFn log = [this](const std::string& line) { m_platform.presetLog(line); };
    const PresetSaveAsFn onSaveAs = [this](const PresetSaveAsRequest& request, std::string* error) {
        switch (request.target) {
            case PresetStep::SaveAsTarget::Slot1:
                return m_modules->saveAsFromPreset(1, request.name, request.path, request.isTemplate, error);
            case PresetStep::SaveAsTarget::Slot2:
                return m_modules->saveAsFromPreset(2, request.name, request.path, request.isTemplate, error);
            case PresetStep::SaveAsTarget::Floppy:
                return m_floppy->saveAsFromPreset(request.name, request.path, request.isTemplate, error);
        }
        return false;
    };

    // The model switch is announced before the preset even runs, so the
    // frontend's presetArmed() handler already sees the new model while the
    // armed machine is still off. Core fires onArmed on every arming
    // outcome, so the bays are synced exactly once, before any `saveas:`
    // step retargets them.
    PresetLoadResult result;
    if (preset.isPC1600()) {
        PC1600Machine& machine = m_machine->resetBareForPresetPC1600(
            preset.romVariant == "old" ? PC1600RomVersion::Old : PC1600RomVersion::New,
            preset.ce1600pRomVariant == "old" ? CE1600PRomVersion::Old : CE1600PRomVersion::New);
        m_machine->finishPresetLoad(Model::PC1600);
        const auto onArmed = [this](const PresetLoadResult& armedSoFar) {
            m_modules->syncFromPresetLoad(1, armedSoFar.slot1ResolvedPath, armedSoFar.slot1RomFile);
            m_modules->syncFromPresetLoad(2, armedSoFar.slot2ResolvedPath, armedSoFar.slot2RomFile);
            m_floppy->syncFromPresetLoad(armedSoFar.floppyImageLabel, armedSoFar.floppyResolvedPath);
            m_platform.presetArmed();
        };
        result = co_await applyPC1600PresetJob(machine, preset, log, traceDir, bundled,
                                               [&machine] { seedClockFromHostTime(machine); }, romDirs,
                                               extraModuleDirs, onArmed, onSaveAs);
    } else {
        PC1500Machine& machine = m_machine->resetBareForPresetPC1500(preset.variant);
        m_machine->finishPresetLoad(modelForPreset(preset));
        const auto onArmed = [this](const PresetLoadResult& armedSoFar) {
            m_modules->syncFromPresetLoad(1, armedSoFar.slot1ResolvedPath, armedSoFar.slot1RomFile);
            m_modules->syncFromPresetLoad(2);
            m_platform.presetArmed();
        };
        result = co_await applyPC1500PresetJob(machine, preset, log, traceDir, bundled,
                                               [&machine] { seedClockFromHostTime(machine); }, romDirs,
                                               extraModuleDirs, onArmed, onSaveAs);
    }
    if (!result.ok) co_return OpResult::fail(result.error);
    co_return OpResult{};
}

job::Task<OpResult> AppSession::loadDefaultPresetJob(std::string path, Model model) {
    PresetFile preset;
    std::string parseError;
    if (!parsePresetFile(path, &preset, &parseError)) co_return OpResult::fail(parseError);
    if (modelForPreset(preset) != model) {
        co_return OpResult::fail("The default preset \"" + path +
                                 "\" is for a different model -- change it in Settings.");
    }
    co_return co_await loadPresetJob(path);
}

job::Task<OpResult> AppSession::resetJob(bool allReset) {
    if (!m_machine->hasLiveMachine()) co_return OpResult::fail("No machine is running.");
    co_await m_machine->resetToPromptJob(allReset);
    co_return OpResult{};
}

job::Task<OpResult> AppSession::selectModelJob(Model model) {
    if (model == m_machine->currentModel()) co_return OpResult{};
    m_modules->flushPendingPersist();
    m_floppy->flushPendingPersist();
    m_modules->onModelChanged();
    // A model switch is a fresh default machine: A04 / new ROM, no plotter,
    // no modules (onModelChanged above), no floppy.
    m_machine->resetRomSelectionsToDefault();
    co_await m_machine->switchModelJob(model);
    m_floppy->selectDisk({});
    co_return OpResult{};
}

job::Task<OpResult> AppSession::defaultPresetJob() {
    const Model model = m_machine->currentModel();
    const std::string path = m_settings.defaultPresetPath(model);
    if (path.empty()) co_return OpResult{};
    co_return co_await loadDefaultPresetJob(path, model);
}

job::Task<OpResult> AppSession::pickModelJob(Model model) {
    if (model == m_machine->currentModel()) co_return OpResult{};
    co_await selectModelJob(model);
    co_return co_await defaultPresetJob();
}

job::Task<OpResult> AppSession::selectModuleJob(int slot, std::string moduleNameOrEmpty) {
    m_modules->selectModule(slot, moduleNameOrEmpty);
    co_await m_machine->switchModelJob(m_machine->currentModel(), /*keepPeripherals=*/true);  // rebuild -> re-attach
    co_return OpResult{};
}

job::Task<OpResult> AppSession::selectPC1500RomJob(PC1500RomRevision revision) {
    if (revision == m_machine->pc1500RomRevision()) co_return OpResult{};
    co_await m_machine->setPC1500RomRevisionJob(revision);  // rebuilds a PC-1500
    co_return OpResult{};
}

job::Task<OpResult> AppSession::selectPC1600RomJob(PC1600RomVersion version) {
    if (version == m_machine->pc1600RomVersion()) co_return OpResult{};
    co_await m_machine->setPC1600RomVersionJob(version);  // rebuilds a PC-1600
    co_return OpResult{};
}

job::Task<OpResult> AppSession::selectCE1600PRomJob(CE1600PRomVersion version) {
    if (version == m_machine->ce1600pRomVersion()) co_return OpResult{};
    if (!m_machine->ce1600pAttached()) {
        m_machine->setCE1600PRomVersion(version);
        co_return OpResult{};
    }
    co_await m_machine->powerCycleAroundJob([this, version] {
        std::string error;
        if (!m_machine->swapCE1600PRom(version, &error)) m_platform.attachFailed("CE-1600P", error);
    });
    co_return OpResult{};
}

job::Task<OpResult> AppSession::togglePeripheralJob(Peripheral p) {
    co_await m_machine->powerCycleAroundJob([this, p] {
        MachineSession& m = *m_machine;
        std::string error;
        bool ok = true;
        switch (p) {
            case Peripheral::CE150:
                if (m.ce150Attached()) m.detachCE150();
                else ok = m.attachCE150(&error);
                break;
            case Peripheral::CE1600P:
                if (m.ce1600pAttached()) m.detachCE1600P();
                else ok = m.attachCE1600P(&error);
                break;
            case Peripheral::CE158:
                if (m.ce158Attached()) m.detachCE158();
                else ok = m.attachCE158(&error);
                break;
        }
        if (!ok) m_platform.attachFailed(peripheralName(p), error);
    });
    co_return OpResult{};
}

job::Task<OpResult> AppSession::mountHostDriveJob(std::string dirOrEmpty) {
    if (dirOrEmpty.empty()) {
        if (!m_machine->hostDriveAttached()) co_return OpResult{};
        co_await m_machine->powerCycleAroundJob([this] { m_machine->detachHostDrive(); });
        co_return OpResult{};
    }
    std::string error;
    if (m_machine->hostDriveAttached()) {
        // Only the directory changes, live.
        if (!m_machine->attachHostDrive(dirOrEmpty, &error)) co_return OpResult::fail(error);
        co_return OpResult{};
    }
    bool attached = false;
    co_await m_machine->powerCycleAroundJob(
        [this, &attached, &error, dirOrEmpty] { attached = m_machine->attachHostDrive(dirOrEmpty, &error); });
    if (!attached) co_return OpResult::fail(error);
    co_return OpResult{};
}

OpResult AppSession::loadBasicProgram(const std::vector<std::uint8_t>& file, const std::string& path) {
    // Live-machine LOAD, like LOAD on the real machine: no reset, MODE
    // change or NEW0 -- the user has prepared the machine as they would
    // before typing LOAD. On the PC-1600 the MODE picks the keyword table
    // and the TITLE area is the target.
    BasicLoadResult loaded;
    if (PC1600Machine* m = m_machine->pc1600()) loaded = ::loadBasicProgram(*m, file);
    else if (PC1500Machine* m = m_machine->pc1500()) loaded = ::loadBasicProgram(*m, file);
    else return OpResult::fail("No machine is running.");
    if (!loaded.ok) return OpResult::fail(path + ": " + loaded.error);
    return OpResult{};
}

OpResult AppSession::loadMachineCode(const MachineCodeLoadRequest& request) {
    std::string error;
    bool ok = false;
    if (m_machine->currentModel() == Model::PC1600) {
        PC1600Machine* m = m_machine->pc1600();
        if (!m) return OpResult::fail("No PC-1600 machine is running.");
        ok = loadPC1600MachineCode(*m, request.slot, request.addr, request.payload.data(), request.payload.size(),
                                   &error, request.bank);
    } else {
        PC1500Machine* m = m_machine->pc1500();
        if (!m) return OpResult::fail("No PC-1500 machine is running.");
        ok = loadPC1500MachineCode(*m, request.addr, request.payload.data(), request.payload.size(), &error);
    }
    return ok ? OpResult{} : OpResult::fail(error);
}

} // namespace shared
