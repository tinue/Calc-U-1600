#include "DebugController.hpp"

#include "AppSettings.hpp"
#include "DapServer.hpp"
#include "DapSession.hpp"
#include "Debug/MachineDebugTargets.hpp"
#include "MachineController.hpp"

DebugController::DebugController(MachineController* machines, QObject* parent)
    : QObject(parent), m_machines(machines) {
    m_server = new DapServer(this);
    connect(m_server, &DapServer::clientConnected, this, &DebugController::onClientConnected);
    connect(m_server, &DapServer::clientDisconnected, this, &DebugController::onClientDisconnected);
    connect(m_server, &DapServer::messageReceived, this, [this](const QJsonObject& message) {
        if (m_session) m_session->handle(message);
    });
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
