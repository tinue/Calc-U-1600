#include "SyncOperations.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QProgressDialog>
#include <QWidget>

#include <memory>

#include "EmulationPacer.hpp"

namespace {
// How often the event loop is pumped between job slices, and how long an
// operation must run before the "Loading…" popup appears (short ones
// finish without flashing it).
constexpr int kPumpIntervalMs = 30;
constexpr int kPopupDelayMs = 500;

job::Task<shared::OpResult> instant(std::function<bool(QString*)> op) {
    QString error;
    const bool ok = op(&error);
    co_return ok ? shared::OpResult{} : shared::OpResult::fail(error.toStdString());
}
} // namespace

SyncOperations::SyncOperations(QWidget* window, MachineController* machines, EmulationPacer* pacer,
                               std::function<void()> refreshLcd, QObject* parent)
    : QObject(parent), m_window(window), m_machines(machines), m_pacer(pacer), m_refreshLcd(std::move(refreshLcd)) {}

bool SyncOperations::run(const QString& title, const std::function<bool(QString*)>& op,
                         const std::function<void()>& afterLoad, QString* error) {
    return run(title, instant(op), afterLoad, error);
}

bool SyncOperations::run(const QString& title, job::Task<shared::OpResult> op, const std::function<void()>& afterLoad,
                         QString* error) {
    auto report = [this, &title, error](const shared::OpResult& r) {
        if (!r.ok) {
            const QString text = QString::fromStdString(r.error);
            if (error) *error = text;
            else QMessageBox::warning(m_window, title, text);
        }
        return r.ok;
    };
    if (m_running) {
        // Already inside one: the timer is stopped; run straight through.
        const shared::OpResult r = m_machines->app().runNow(std::move(op));
        if (afterLoad) afterLoad();
        return report(r);
    }

    m_pacer->suspend();
    m_running = true;
    emit busyChanged(true);
    m_window->setCursor(Qt::WaitCursor);

    // Between job slices: keep the window painting (no beachball) and, once
    // the operation has run long enough to be noticeable, show a
    // "Loading…" popup. User input stays excluded -- nothing may touch the
    // machine until the operation returns.
    QElapsedTimer sinceStart;
    QElapsedTimer sincePump;
    sinceStart.start();
    sincePump.start();
    std::unique_ptr<QProgressDialog> popup;
    const auto pump = [&] {
        if (sincePump.elapsed() < kPumpIntervalMs) return;
        sincePump.restart();
        if (!popup && sinceStart.elapsed() >= kPopupDelayMs) {
            popup = std::make_unique<QProgressDialog>(tr("Loading…"), QString(), 0, 0, m_window);
            popup->setWindowTitle(title);
            popup->setWindowModality(Qt::WindowModal);
            popup->setMinimumDuration(0);
            popup->show();
        }
        // Let the LCD follow along too (the frame timer is stopped).
        if (m_refreshLcd) m_refreshLcd();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    };

    const shared::OpResult r = m_machines->app().runNow(std::move(op), pump);
    popup.reset();
    m_window->unsetCursor();
    if (afterLoad) afterLoad();
    m_pacer->resume();
    m_running = false;
    emit busyChanged(false);
    return report(r);
}

bool SyncOperations::loadPreset(const QString& path, QString* error, bool armOnly) {
    return run(tr("Load Preset"), m_machines->app().loadPresetJob(path.toStdString(), armOnly), m_presetResync, error);
}

bool SyncOperations::loadDefaultPreset(const QString& path, Model model, QString* error) {
    return run(tr("Default Preset"), m_machines->app().loadDefaultPresetJob(path.toStdString(), model),
               m_presetResync, error);
}

bool SyncOperations::resetToPrompt(bool allReset, QString* error) {
    // Boot flat out to the prompt (incl. a plotter's power-on init) instead
    // of watching it in real time; the clock is set from the host after.
    return run(allReset ? tr("Reset All") : tr("Reset"), m_machines->app().resetJob(allReset), {}, error);
}
