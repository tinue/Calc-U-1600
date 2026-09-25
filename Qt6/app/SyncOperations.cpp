#include "SyncOperations.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QProgressDialog>
#include <QWidget>

#include <memory>

#include "EmulationPacer.hpp"
#include "FloppyDiskManager.hpp"
#include "MemoryModuleManager.hpp"
#include "PresetController.hpp"

namespace {
// How often the blocked UI thread pumps its event loop mid-operation, and
// how long one must run before the "Loading…" popup appears (short ones
// finish without flashing it).
constexpr int kPumpIntervalMs = 30;
constexpr int kPopupDelayMs = 500;
} // namespace

SyncOperations::SyncOperations(QWidget* window, MachineController* machines, PresetController* presets,
                               EmulationPacer* pacer, MemoryModuleManager* modules, FloppyDiskManager* floppies,
                               std::function<void()> refreshLcd, QObject* parent)
    : QObject(parent), m_window(window), m_machines(machines), m_presets(presets), m_pacer(pacer), m_modules(modules),
      m_floppies(floppies), m_refreshLcd(std::move(refreshLcd)) {}

bool SyncOperations::run(const QString& title, const std::function<bool(QString*)>& op,
                         const std::function<void()>& afterLoad, QString* error) {
    auto report = [this, &title, error](bool ok, const QString& opError) {
        if (!ok) {
            if (error) *error = opError;
            else QMessageBox::warning(m_window, title, opError);
        }
        return ok;
    };
    QString opError;
    if (m_depth > 0) {
        // Already inside one: the timer is stopped and the hook installed.
        const bool ok = op(&opError);
        if (afterLoad) afterLoad();
        return report(ok, opError);
    }

    m_pacer->suspend();
    m_depth++;
    emit busyChanged(true);
    m_modules->flushPendingPersist();
    m_floppies->flushPendingPersist();
    m_window->setCursor(Qt::WaitCursor);

    // The operation blocks this thread, so pump the event loop from the
    // machine's yield hook (see PresetController::setYieldHook()): keeps the
    // window painting (no beachball) and, once it has run long enough to be
    // noticeable, shows a "Loading…" popup. User input stays excluded --
    // nothing may touch the machine until the operation returns.
    QElapsedTimer sinceStart;
    QElapsedTimer sincePump;
    sinceStart.start();
    sincePump.start();
    std::unique_ptr<QProgressDialog> popup;
    m_presets->setYieldHook([&] {
        if (sincePump.elapsed() < kPumpIntervalMs) return;
        sincePump.restart();
        if (!popup && sinceStart.elapsed() >= kPopupDelayMs) {
            popup = std::make_unique<QProgressDialog>(tr("Loading…"), QString(), 0, 0, m_window);
            popup->setWindowTitle(title);
            popup->setWindowModality(Qt::WindowModal);
            popup->setMinimumDuration(0);
            popup->show();
        }
        // Let the LCD follow along too (the frame timer that normally
        // refreshes it is stopped).
        if (m_refreshLcd) m_refreshLcd();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    });

    const bool ok = op(&opError);
    m_presets->setYieldHook({});
    popup.reset();
    m_window->unsetCursor();
    if (afterLoad) afterLoad();
    // The operation ran the machine flat out -- whatever it beeped is stale.
    m_machines->discardAudio();
    m_pacer->resume();
    m_depth--;
    emit busyChanged(false);
    return report(ok, opError);
}

bool SyncOperations::loadPreset(const QString& path, QString* error) {
    return run(
        tr("Load Preset"), [this, path](QString* e) { return m_presets->loadPreset(path, e); }, m_presetResync, error);
}

bool SyncOperations::loadDefaultPreset(const QString& path, Model model, QString* error) {
    return run(
        tr("Default Preset"), [this, path, model](QString* e) { return m_presets->loadDefaultPreset(path, model, e); },
        m_presetResync, error);
}

bool SyncOperations::resetToPrompt(bool allReset, QString* error) {
    // Boot flat out to the prompt (incl. a plotter's power-on init) instead
    // of watching it in real time; the clock is set from the host after.
    return run(
        allReset ? tr("Reset All") : tr("Reset"), [this, allReset](QString* e) { return m_presets->resetLive(allReset, e); },
        {}, error);
}
