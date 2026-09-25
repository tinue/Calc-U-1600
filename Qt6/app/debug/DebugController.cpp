#include "DebugController.hpp"

#include <QTimer>

#include <algorithm>

#include "AppSettings.hpp"
#include "DapServer.hpp"
#include "DapSession.hpp"
#include "Debug/MachineDebugTargets.hpp"
#include "MachineController.hpp"
#include "PC1500/PC1500Machine.hpp"
#include "PC1600/PC1600Machine.hpp"

DebugController::DebugController(MachineController* machines, QObject* parent)
    : QObject(parent), m_machines(machines) {
    m_server = new DapServer(this);
    connect(m_server, &DapServer::clientConnected, this, &DebugController::onClientConnected);
    connect(m_server, &DapServer::clientDisconnected, this, &DebugController::onClientDisconnected);
    connect(m_server, &DapServer::messageReceived, this, &DebugController::dispatch);
}

void DebugController::dispatch(const QJsonObject& message) {
    // A synchronous load pumps the event loop: messages arriving meanwhile
    // (or while a request is being handled) wait, in order.
    m_queued.push_back(message);
    drainQueue();
}

void DebugController::drainQueue() {
    if (m_busy || m_appBusy > 0) return;
    m_busy = true;
    while (!m_queued.empty() && m_appBusy == 0) {
        const QJsonObject next = m_queued.front();
        m_queued.erase(m_queued.begin());
        if (m_session) m_session->handle(next);
    }
    m_busy = false;
}

void DebugController::setAppBusy(bool busy) {
    m_appBusy = std::max(0, m_appBusy + (busy ? 1 : -1));
    if (m_appBusy == 0 && !m_queued.empty()) QTimer::singleShot(0, this, &DebugController::drainQueue);
}

DebugController::~DebugController() {
    endSession();
    m_session.reset();
}

// ── Server ────────────────────────────────────────────────────────────────

namespace {
int s_commandLinePort = 0; // --dap <port>; 0 = use Settings

bool serverWanted() { return s_commandLinePort > 0 || AppSettings::dapEnabled(); }
int serverPort() { return s_commandLinePort > 0 ? s_commandLinePort : AppSettings::dapPort(); }
} // namespace

void DebugController::setCommandLinePort(int port) { s_commandLinePort = port >= 1024 && port <= 65535 ? port : 0; }

void DebugController::refreshServer() {
    m_serverError.clear();
    if (!serverWanted()) {
        m_server->close();
    } else if (!m_server->isListening() || m_server->port() != serverPort()) {
        QString error;
        if (!m_server->listen(quint16(serverPort()), &error)) m_serverError = error;
    }
    emit serverStatusChanged();
}

QString DebugController::serverStatus() const {
    if (!serverWanted()) return tr("Off");
    if (!m_server->isListening())
        return tr("Port %1 not available: %2").arg(serverPort()).arg(m_serverError);
    if (m_server->hasClient()) return tr("Client connected on 127.0.0.1:%1").arg(m_server->port());
    return tr("Listening on 127.0.0.1:%1").arg(m_server->port());
}

bool DebugController::hasClient() const { return m_server->hasClient(); }

void DebugController::disconnectClient() {
    if (m_session) {
        m_session->output(tr("Disconnected from the app (Settings > Debugger)."), QStringLiteral("important"));
        m_session->terminated();
    }
    m_server->disconnectClient(); // -> onClientDisconnected(): session ends, machine runs on
}

void DebugController::onClientConnected() {
    m_session = std::make_unique<DapSession>(m_server, this);
    emit serverStatusChanged();
}

void DebugController::onClientDisconnected() {
    endSession();
    m_session.reset();
    emit serverStatusChanged();
}

// ── Session ───────────────────────────────────────────────────────────────

void DebugController::createTarget() {
    m_run.reset();
    m_target.reset();
    if (PC1500Machine* m = m_machines->pc1500()) m_target = std::make_unique<debug::PC1500DebugTarget>(*m);
    else if (PC1600Machine* m = m_machines->pc1600()) m_target = std::make_unique<debug::PC1600DebugTarget>(*m);
    if (m_target) m_run = std::make_unique<debug::RunControl>(*m_target, m_map, m_breakpoints);
}

bool DebugController::beginSession() {
    m_map.clear();
    m_breakpoints.clear();
    createTarget();
    m_sessionActive = m_target != nullptr;
    if (m_target) m_target->arm(false);
    return m_sessionActive;
}

void DebugController::endSession() {
    // Dropping the target clears its breakpoints and watches from the CPUs;
    // the machine then runs on normally.
    m_sessionActive = false;
    m_run.reset();
    m_target.reset();
    m_breakpoints.clear();
    m_map.clear();
    if (m_lastPaused) {
        m_lastPaused = false;
        emit pausedChanged(false);
    }
}

bool DebugController::attached() const { return m_sessionActive; }

bool DebugController::paused() const { return m_sessionActive && m_run && m_run->paused(); }

void DebugController::runSlice(std::uint64_t cycles) {
    if (!m_sessionActive) return;
    if (!m_target) {
        // The machine was rebuilt (or reset flat out) since the last frame:
        // bind to the new one now that nothing else drives it.
        createTarget();
        if (!m_target) return;
        m_run->resume();
        if (m_session) m_session->onMachineReplaced();
    }
    // Breakpoints and watches act only while the debugger runs the machine;
    // a boot or load in between must not park on one.
    m_target->arm(true);
    const std::vector<debug::DebugEvent> events = m_run->slice(cycles);
    m_target->arm(false);
    if (m_session && !events.empty()) m_session->onEvents(events);
    const bool nowPaused = m_run->paused();
    if (nowPaused != m_lastPaused) {
        m_lastPaused = nowPaused;
        emit pausedChanged(nowPaused);
    }
}

std::vector<debug::BreakpointStatus> DebugController::rebindListings() {
    if (!m_run) return {};
    for (const auto& b : m_map.bindings()) m_map.verify(b.id, m_run->bankMatch(), m_run->codePeek());
    auto changed = m_breakpoints.reresolve(m_map, 1);
    if (m_target) {
        const bool armed = m_target->armed();
        m_breakpoints.apply(*m_target);
        m_target->arm(armed);
    }
    return changed;
}

bool DebugController::loadPreset(const QString& path, QString* error) {
    if (!m_presetLoader) {
        *error = tr("Presets can't be loaded from the debugger here");
        return false;
    }
    const bool ok = m_presetLoader(path, error);
    // The preset rebuilt the machine: bind to it right away (paused, as a
    // fresh session is).
    if (m_sessionActive && !m_target) createTarget();
    return ok;
}

bool DebugController::cleanStart(const QString& preset, QString* how, QString* error) {
    QString path = preset;
    if (path.isEmpty()) path = AppSettings::defaultPresetPath(modelSettingsKey(m_machines->currentModel()));
    if (!path.isEmpty()) {
        *how = tr("preset %1").arg(path);
        return loadPreset(path, error);
    }
    // No preset: the machine as Reset All leaves it, booted to the prompt.
    *how = tr("All Reset (no default preset for this model)");
    m_machines->resetToPrompt(/*allReset=*/true);
    if (m_sessionActive && !m_target) createTarget();
    return true;
}

debug::LoadResult DebugController::loadProgram(const debug::LoadRequest& request, After after) {
    debug::LoadResult r;
    if (!m_target || !m_run) {
        r.error = "no machine";
        return r;
    }
    r = debug::loadProgram(m_machines->pc1500(), m_machines->pc1600(), request, m_map, *m_target);
    if (!r.ok) return r;
    if (after == After::None) return r;
    if (r.callCommand.empty()) {
        r.warnings.push_back("no BASIC CALL starts this CPU's code; load it, then call it from your own code");
        return r;
    }
    if (after == After::StopOnEntry) m_breakpoints.setEntry(request.thread, r.entry);
    m_breakpoints.apply(*m_target);
    m_target->arm(false);
    m_machines->typeCommand(r.callCommand); // typed and entered as the machine runs
    m_run->resume();
    return r;
}

bool DebugController::resetMachine(bool allReset, bool stop, QString* error) {
    if (!m_target || !m_run) {
        *error = tr("No machine is running");
        return false;
    }
    m_machines->cancelPaste();
    m_target->reset(allReset);
    if (stop) m_run->pause(debug::DebugEvent::Entry);
    else m_run->resume();
    return true;
}

void DebugController::machineAboutToChange() {
    // The target refers to the machine that is about to go; runSlice()
    // binds to its successor.
    m_run.reset();
    m_target.reset();
    if (m_lastPaused) {
        m_lastPaused = false;
        emit pausedChanged(false);
    }
}
