#include "PresetController.hpp"

#include <functional>

#include "PC1500/PC1500Machine.hpp"
#include "PC1500/PC1500MachineCodeLoader.hpp"
#include "PC1600/PC1600Machine.hpp"
#include "PC1600/PC1600MachineCodeLoader.hpp"

namespace {

// Installs PresetController's yield hook on `machine` for the lifetime of
// this object. ~10 ms of emulated time between calls: frequent enough for a
// smooth UI at any emulation speed; the hook itself rate-limits by wall
// clock. Templated over the two machine classes.
template <typename Machine>
class ScopedYieldHook {
public:
    ScopedYieldHook(Machine& machine, const std::function<void()>& hook, double clockHz)
        : m_machine(machine), m_active(static_cast<bool>(hook)) {
        if (m_active) m_machine.setYieldHook(hook, static_cast<uint64_t>(clockHz / 100));
    }
    ~ScopedYieldHook() {
        if (m_active) m_machine.setYieldHook({}, 0);
    }
    ScopedYieldHook(const ScopedYieldHook&) = delete;
    ScopedYieldHook& operator=(const ScopedYieldHook&) = delete;

private:
    Machine& m_machine;
    bool m_active;
};

}  // namespace

PresetController::PresetController(MachineController* controller, MemoryModuleManager* moduleManager,
                                     FloppyDiskManager* floppyManager, QObject* parent)
    : QObject(parent), m_controller(controller), m_moduleManager(moduleManager), m_floppyManager(floppyManager) {}

bool PresetController::withYieldHook(const std::function<void()>& body, QString* error) {
    if (PC1600Machine* machine = m_controller->pc1600()) {
        const ScopedYieldHook<PC1600Machine> yieldHook(*machine, m_yieldHook, m_controller->clockHz());
        body();
    } else if (PC1500Machine* machine = m_controller->pc1500()) {
        const ScopedYieldHook<PC1500Machine> yieldHook(*machine, m_yieldHook, m_controller->clockHz());
        body();
    } else {
        *error = tr("No machine is running.");
        return false;
    }
    return true;
}

bool PresetController::resetLive(bool allReset, QString* error) {
    return withYieldHook([this, allReset] { m_controller->resetToPrompt(allReset); }, error);
}

bool PresetController::powerCycleLive(const std::function<void()>& change, QString* error) {
    return withYieldHook([this, &change] { m_controller->powerCycleAround(change); }, error);
}

bool PresetController::loadMachineCodeLive(const MachineCodeLoadRequest& request, QString* error) {
    std::string err;
    bool ok = false;
    if (m_controller->currentModel() == Model::PC1600) {
        PC1600Machine* machine = m_controller->pc1600();
        if (!machine) {
            *error = tr("No PC-1600 machine is running.");
            return false;
        }
        ok = loadPC1600MachineCode(*machine, request.slot, request.addr, request.payload.data(),
                                   request.payload.size(), &err);
    } else {
        PC1500Machine* machine = m_controller->pc1500();
        if (!machine) {
            *error = tr("No PC-1500 machine is running.");
            return false;
        }
        ok = loadPC1500MachineCode(*machine, request.addr, request.payload.data(), request.payload.size(), &err);
    }
    if (!ok) *error = QString::fromStdString(err);
    return ok;
}

#ifdef CALCU1600_PRESET_LOADER_AVAILABLE

#include "MachineController.hpp"
#include "MemoryModuleManager.hpp"
#include "FloppyDiskManager.hpp"
#include "AppPaths.hpp"
#include "AppSettings.hpp"

#include <QDebug>
#include <vector>

#include "Preset/PresetFile.hpp"
#include "PC1500/PC1500PresetLoader.hpp"
#include "PC1500/PC1500Machine.hpp"
#include "PC1500/PC1500BasicLoader.hpp"
#include "PC1600/PC1600PresetLoader.hpp"
#include "PC1600/PC1600Machine.hpp"
#include "PC1600/PC1600BasicLoader.hpp"
#include "Basic/BasicProgramSource.hpp"
#include "HostClock.hpp"

namespace {

Model modelForPreset(const PresetFile& preset) {
    if (preset.isPC1600()) return Model::PC1600;
    return preset.variant == PC1500Variant::PC1500A ? Model::PC1500A : Model::PC1500;
}

// Live-machine LOAD: mirrors real hardware LOAD semantics, not NEW+type. No
// reset, no mode change, no NEW0 typed here -- the user is expected to have
// already prepared the machine themselves (memory cards, `NEW`, mode,
// peripherals), exactly as they would before typing LOAD on a real machine.
// loadBasicBinaryPayload() validates whatever BASPRG_ST/BASPRG_END are
// currently live, erases the resident program between them, and pokes the
// new one in from BASPRG_ST -- see its own header doc comment.
template <class Machine>
bool loadBasicProgramLiveOn(Machine& machine, basic::TransferModel model, const std::string& path,
                            QString* error) {
    basic::BasicProgramSource src = basic::readBasicProgramSource(path, model);
    if (!src.ok) {
        *error = QString::fromStdString(src.error);
        return false;
    }
    BasicLoadResult loaded = loadBasicBinaryPayload(machine, src.payload);
    if (!loaded.ok) {
        *error = QString::fromStdString(loaded.error);
        return false;
    }
    return true;
}

}  // namespace

namespace {

bool parsePreset(const QString& path, PresetFile* preset, QString* error) {
    std::string parseError;
    if (!parsePresetFile(path.toStdString(), preset, &parseError)) {
        *error = QString::fromStdString(parseError);
        return false;
    }
    return true;
}

}  // namespace

bool PresetController::loadPreset(const QString& path, QString* error) {
    PresetFile preset;
    return parsePreset(path, &preset, error) && runPreset(preset, error);
}

bool PresetController::loadDefaultPreset(const QString& path, Model model, QString* error) {
    PresetFile preset;
    if (!parsePreset(path, &preset, error)) return false;
    if (modelForPreset(preset) != model) {
        *error = tr("The default preset \"%1\" is for a different model -- change it in Settings.").arg(path);
        return false;
    }
    return runPreset(preset, error);
}

namespace {

// Preset `saveas:` dispatch -- shared by the PC-1600 and PC-1500 branches
// below (the PC-1500 side only ever sees SaveAsTarget::S1, per
// PresetFile.cpp's per-model validation).
bool saveAsFromPreset(MemoryModuleManager* moduleManager, FloppyDiskManager* floppyManager,
                       PresetStep::SaveAsTarget target, const std::string& name, std::string* error) {
    QString qerror;
    const QString qname = QString::fromStdString(name);
    bool ok = false;
    switch (target) {
        case PresetStep::SaveAsTarget::S1:
            ok = moduleManager->saveAsFromPreset(1, qname, &qerror);
            break;
        case PresetStep::SaveAsTarget::S2:
            ok = moduleManager->saveAsFromPreset(2, qname, &qerror);
            break;
        case PresetStep::SaveAsTarget::Floppy:
            ok = floppyManager->saveAsFromPreset(qname, &qerror);
            break;
    }
    if (!ok && error) *error = qerror.toStdString();
    return ok;
}

}  // namespace

struct PresetController::PresetEnv {
    std::vector<std::string> romDirs;
    std::string moduleDir;
    std::vector<std::string> extraModuleDirs;
    std::string traceDir;
    PresetLogFn log;
    PresetSaveAsFn onSaveAs;
};

bool PresetController::runPreset(const PresetFile& preset, QString* error) {
    // Bundled catalog first, then the user's writable instance directory --
    // the same order MemoryModuleManager's own attachOneSlot() uses, so a
    // preset's `- modulespec: <name>` resolves identically to the live
    // module picker. The same bundled directory also holds the ROM images
    // Core/Resources/BundledRomCatalog.hpp resolves by name.
    PresetEnv env;
    const std::string bundledDir = AppPaths::bundledResourcesDir().toStdString();
    env.romDirs = {bundledDir};
    env.moduleDir = bundledDir;
    env.extraModuleDirs = {AppPaths::instanceDir().toStdString()};
    // `- trace:` and `- screenshot:` steps write into the Settings trace
    // directory, falling back to the instance directory (always present and
    // writable) -- the same resolution DebugPanel's trace capture uses.
    const QString traceDirSetting = AppSettings::traceDirOverride();
    env.traceDir = (traceDirSetting.isEmpty() ? AppPaths::instanceDir() : traceDirSetting).toStdString();
    env.log = [](const std::string& line) { qDebug().noquote() << "[preset]" << QString::fromStdString(line); };
    env.onSaveAs = [this](PresetStep::SaveAsTarget target, const std::string& name, std::string* saveError) {
        return ::saveAsFromPreset(m_moduleManager, m_floppyManager, target, name, saveError);
    };

    const PresetLoadResult result = preset.isPC1600() ? runPC1600Preset(preset, env) : runPC1500Preset(preset, env);
    if (!result.ok) {
        *error = QString::fromStdString(result.error);
        return false;
    }
    return true;
}

// Both run methods announce the model switch before apply*Preset() even
// runs, not after -- MainWindow's `armed()` handler needs
// MachineController::currentModel() to already read the new model so it
// paints the right faceplate/control bar while the machine is still off.
// Core fires onArmed on every arming outcome (see PresetArmedFn), so the
// pickers are synced exactly once, before any `saveas:` step retargets
// them.

PresetLoadResult PresetController::runPC1500Preset(const PresetFile& preset, const PresetEnv& env) {
    PC1500Machine& machine = m_controller->resetBareForPresetPC1500(preset.variant);
    const ScopedYieldHook<PC1500Machine> yieldHook(machine, m_yieldHook, m_controller->clockHz());
    m_controller->finishPresetLoad(modelForPreset(preset));
    const auto onArmed = [this](const PresetLoadResult& armedSoFar) {
        m_moduleManager->syncFromPresetLoad(1, QString::fromStdString(armedSoFar.slot1ResolvedPath));
        m_moduleManager->syncFromPresetLoad(2);
        emit armed();
    };
    return applyPC1500Preset(machine, preset, env.log, env.traceDir, env.moduleDir,
                             [&machine] { seedClockFromHostTime(machine); }, env.romDirs, env.extraModuleDirs,
                             onArmed, env.onSaveAs);
}

PresetLoadResult PresetController::runPC1600Preset(const PresetFile& preset, const PresetEnv& env) {
    PC1600Machine& machine = m_controller->resetBareForPresetPC1600(
        preset.romVariant == "old" ? PC1600RomVersion::Old : PC1600RomVersion::New,
        preset.ce1600pRomVariant == "old" ? CE1600PRomVersion::Old : CE1600PRomVersion::New);
    const ScopedYieldHook<PC1600Machine> yieldHook(machine, m_yieldHook, m_controller->clockHz());
    m_controller->finishPresetLoad(Model::PC1600);
    const auto onArmed = [this](const PresetLoadResult& armedSoFar) {
        m_moduleManager->syncFromPresetLoad(1, QString::fromStdString(armedSoFar.slot1ResolvedPath));
        m_moduleManager->syncFromPresetLoad(2, QString::fromStdString(armedSoFar.slot2ResolvedPath));
        m_floppyManager->syncFromPresetLoad(QString::fromStdString(armedSoFar.floppyImageLabel),
                                            QString::fromStdString(armedSoFar.floppyResolvedPath));
        emit armed();
    };
    return applyPC1600Preset(machine, preset, env.log, env.traceDir, env.moduleDir,
                             [&machine] { seedClockFromHostTime(machine); }, env.romDirs, env.extraModuleDirs,
                             onArmed, env.onSaveAs);
}

bool PresetController::loadBasicProgramLive(const QString& path, QString* error) {
    if (m_controller->currentModel() == Model::PC1600) {
        PC1600Machine* machine = m_controller->pc1600();
        if (!machine) {
            *error = tr("No PC-1600 machine is running.");
            return false;
        }
        const ScopedYieldHook<PC1600Machine> yieldHook(*machine, m_yieldHook, m_controller->clockHz());
        return loadBasicProgramLiveOn(*machine, basic::TransferModel::PC1600, path.toStdString(), error);
    }
    PC1500Machine* machine = m_controller->pc1500();
    if (!machine) {
        *error = tr("No PC-1500 machine is running.");
        return false;
    }
    const ScopedYieldHook<PC1500Machine> yieldHook(*machine, m_yieldHook, m_controller->clockHz());
    return loadBasicProgramLiveOn(*machine, basic::TransferModel::PC1500, path.toStdString(), error);
}

#else  // !CALCU1600_PRESET_LOADER_AVAILABLE

bool PresetController::loadPreset(const QString&, QString* error) {
    *error = tr("Preset loading isn't available in this build yet (it currently requires the macOS build -- "
                "see Qt6/CMakeLists.txt).");
    return false;
}

bool PresetController::loadDefaultPreset(const QString& path, Model, QString* error) {
    return loadPreset(path, error);
}

bool PresetController::loadBasicProgramLive(const QString&, QString* error) {
    *error = tr("Loading a BASIC program isn't available in this build yet (it currently requires the macOS "
                "build -- see Qt6/CMakeLists.txt).");
    return false;
}

#endif
