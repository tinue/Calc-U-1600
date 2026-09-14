#include "PresetController.hpp"

PresetController::PresetController(MachineController* controller, MemoryModuleManager* moduleManager,
                                     QObject* parent)
    : QObject(parent), m_controller(controller), m_moduleManager(moduleManager) {}

#ifdef CALCU1600_PRESET_LOADER_AVAILABLE

#include "MachineController.hpp"
#include "MemoryModuleManager.hpp"
#include "AppPaths.hpp"

#include <QDateTime>
#include <QDebug>
#include <vector>

#include "PC1500/PresetFile.hpp"
#include "PC1500/PC1500PresetLoader.hpp"
#include "PC1500/PC1500Machine.hpp"
#include "PC1600/PC1600PresetLoader.hpp"
#include "PC1600/PC1600Machine.hpp"

namespace {

// Seeds `machine`'s RTC from the host's wall-clock time -- must run via
// PresetBootedFn (right after reset/allReset settles, before any of the
// preset's own keys:/program: steps), NOT after applyPreset() returns:
// MachineController::finishPresetLoad()'s own seedClockFromHost() call
// runs too late for this -- a preset step that saves a file (e.g. `FILES
// "S2:"` after a `SAVE`) stamps it with whatever the RTC held at that
// moment, which without this would still be the machine's un-seeded
// power-on default (1/1/00:00), not "now". Templated so one definition
// covers both PC1500Machine::seedClock and PC1600Machine::seedClock
// (same signature, no shared base).
template <typename Machine>
void seedClockFromHost(Machine& machine) {
    const QDateTime now = QDateTime::currentDateTime();
    machine.seedClock(now.date().year(), now.date().month(), now.date().day(), now.time().hour(),
                       now.time().minute(), now.time().second());
}

}  // namespace

bool PresetController::loadPreset(const QString& path, QString* error) {
    PresetFile preset;
    std::string parseError;
    if (!parsePresetFile(path.toStdString(), &preset, &parseError)) {
        *error = QString::fromStdString(parseError);
        return false;
    }

    // Bundled catalog first, then the user's writable instance directory --
    // the same order MemoryModuleManager's own attachOneSlot() uses, so a
    // preset's `- modulespec: <name>` resolves identically to the live
    // module picker. The same bundled directory also holds the ROM images
    // Core/Resources/BundledRomCatalog.hpp resolves by name.
    const std::string bundledDir = AppPaths::bundledResourcesDir().toStdString();
    const std::vector<std::string> romDirs = {bundledDir};
    const std::string moduleDir = bundledDir;
    const std::vector<std::string> extraModuleDirs = {AppPaths::instanceDir().toStdString()};
    // No dedicated trace-directory setting exists yet (the debug area is
    // the last item on this prototype's feature-parity roadmap) -- reuse
    // the instance directory, which is always present and writable, as a
    // reasonable default a `- trace:` step can write into meanwhile.
    const std::string traceDir = AppPaths::instanceDir().toStdString();
    const auto logSink = [](const std::string& line) {
        qDebug().noquote() << "[preset]" << QString::fromStdString(line);
    };

    if (preset.isPC1600()) {
        PC1600Machine& machine = m_controller->resetBareForPresetPC1600();
        // Announce the model switch before applyPC1600Preset() even runs,
        // not after -- MainWindow's `armed()` handler (below) needs
        // MachineController::currentModel() to already read PC-1600 so it
        // paints the right faceplate/control-bar while the machine is still
        // powered off.
        m_controller->finishPresetLoad(Model::PC1600);
        const auto onArmed = [this](const PC1600PresetLoadResult& armedSoFar) {
            m_moduleManager->syncFromPresetLoad(1, QString::fromStdString(armedSoFar.slot1ModuleLabel),
                                                QString::fromStdString(armedSoFar.slot1ResolvedPath));
            m_moduleManager->syncFromPresetLoad(2, QString::fromStdString(armedSoFar.slot2ModuleLabel),
                                                QString::fromStdString(armedSoFar.slot2ResolvedPath));
            emit armed();
        };
        const PC1600PresetLoadResult result =
            applyPC1600Preset(machine, preset, logSink, traceDir, moduleDir,
                               [&machine] { seedClockFromHost(machine); }, romDirs, extraModuleDirs,
                               onArmed);
        // Safety net for a preset that fails before ever arming (bad
        // modulespec, missing plotter ROM, ...) -- onArmed never fired, so
        // the machine's actually-empty slots (resetBareForPresetPC1600()
        // already swapped in a bare machine) still need reflecting into the
        // module manager instead of leaving it showing the previous load's
        // labels. A no-op duplicate of onArmed's own sync otherwise.
        m_moduleManager->syncFromPresetLoad(1, QString::fromStdString(result.slot1ModuleLabel),
                                            QString::fromStdString(result.slot1ResolvedPath));
        m_moduleManager->syncFromPresetLoad(2, QString::fromStdString(result.slot2ModuleLabel),
                                            QString::fromStdString(result.slot2ResolvedPath));
        if (!result.ok) {
            *error = QString::fromStdString(result.error);
            return false;
        }
        return true;
    }

    PC1500Machine& machine = m_controller->resetBareForPresetPC1500(preset.variant);
    const Model model = (preset.variant == PC1500Variant::PC1500A) ? Model::PC1500A : Model::PC1500;
    // Announce the model switch before applyPC1500Preset() even runs, not
    // after -- see the matching comment in the PC-1600 branch above.
    m_controller->finishPresetLoad(model);
    const auto onArmed = [this](const PresetLoadResult& armedSoFar) {
        m_moduleManager->syncFromPresetLoad(1, QString::fromStdString(armedSoFar.expansionModuleLabel),
                                            QString::fromStdString(armedSoFar.expansionModuleResolvedPath));
        m_moduleManager->syncFromPresetLoad(2, QString());
        emit armed();
    };
    const PresetLoadResult result =
        applyPC1500Preset(machine, preset, logSink, traceDir, moduleDir,
                           [&machine] { seedClockFromHost(machine); }, romDirs, extraModuleDirs, onArmed);
    // Safety net for a preset that fails before ever arming -- see the
    // matching comment in the PC-1600 branch above.
    m_moduleManager->syncFromPresetLoad(1, QString::fromStdString(result.expansionModuleLabel),
                                        QString::fromStdString(result.expansionModuleResolvedPath));
    m_moduleManager->syncFromPresetLoad(2, QString());
    if (!result.ok) {
        *error = QString::fromStdString(result.error);
        return false;
    }
    return true;
}

#else  // !CALCU1600_PRESET_LOADER_AVAILABLE

bool PresetController::loadPreset(const QString&, QString* error) {
    *error = tr("Preset loading isn't available in this build yet (it currently requires the macOS build -- "
                "see Qt6/CMakeLists.txt).");
    return false;
}

#endif
