#pragma once
#include <QObject>
#include <QString>

#include <memory>

#include "Debug/BreakpointTable.hpp"
#include "Debug/DebugTarget.hpp"
#include "Debug/RunControl.hpp"
#include "Debug/SourceMap.hpp"

class DapServer;
class DapSession;
class MachineController;

// The app side of the debugger. Owns the DAP server (enabled in Settings)
// and, while a client is attached, the Core debug objects for the live
// machine: its DebugTarget, the source map, the breakpoint table and the
// run control. MachineController routes every frame's cycle budget through
// runSlice() while attached, so pause/step/breakpoints act on the normal
// paced emulation; a machine rebuild swaps the target underneath and
// re-applies everything.
class DebugController : public QObject {
    Q_OBJECT
public:
    explicit DebugController(MachineController* machines, QObject* parent = nullptr);
    ~DebugController() override;

    /// Starts, stops or rebinds the server from the Settings values (or
    /// the --dap command-line port, which overrides them for the run).
    void refreshServer();
    static void setCommandLinePort(int port);
    /// "Listening on 127.0.0.1:4711", "Client connected", "Port in use: …",
    /// "Off".
    QString serverStatus() const;

    /// A debug session is attached: the frame budget goes to runSlice().
    bool attached() const;
    /// Attached and stopped (window title, DebugPanel).
    bool paused() const;
    void runSlice(std::uint64_t cycles);

    /// Called by MachineController before it destroys the live machine.
    void machineAboutToChange();

    // ── For DapSession ────────────────────────────────────────────────────
    /// Creates the target / run control for the live machine; false if
    /// there is none.
    bool beginSession();
    void endSession();
    debug::DebugTarget* target() const { return m_target.get(); }
    debug::RunControl* runControl() const { return m_run.get(); }
    debug::SourceMap& sourceMap() { return m_map; }
    debug::BreakpointTable& breakpoints() { return m_breakpoints; }
    MachineController* machines() const { return m_machines; }
    /// Verifies every listing binding against memory and re-resolves the
    /// breakpoints; returns the breakpoint statuses that changed.
    std::vector<debug::BreakpointStatus> rebindListings();

signals:
    void serverStatusChanged();
    void pausedChanged(bool paused);

private:
    void createTarget();
    void onClientConnected();
    void onClientDisconnected();

    MachineController* m_machines; // not owned
    DapServer* m_server = nullptr;
    std::unique_ptr<DapSession> m_session;
    QString m_serverError;

    std::unique_ptr<debug::DebugTarget> m_target;
    std::unique_ptr<debug::RunControl> m_run;
    debug::SourceMap m_map;
    debug::BreakpointTable m_breakpoints;
    bool m_sessionActive = false;
    bool m_lastPaused = false;
};
