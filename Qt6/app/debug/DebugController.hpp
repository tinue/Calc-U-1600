#pragma once
#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

#include <QJsonObject>

#include "Debug/BreakpointTable.hpp"
#include "Debug/DebugTarget.hpp"
#include "Debug/ProgramLoader.hpp"
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
    bool hasClient() const;
    /// Drops the attached client (Settings > Debugger > Disconnect); the
    /// machine runs on as without a debugger.
    void disconnectClient();
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
    /// Verifies every listing binding against memory and re-resolves the
    /// breakpoints; returns the breakpoint statuses that changed.
    std::vector<debug::BreakpointStatus> rebindListings();

    /// Loads a preset the way File > Load Preset does (MainWindow supplies
    /// it: the frame timer stops for the synchronous load).
    void setPresetLoader(std::function<bool(const QString& path, QString* error)> loader) {
        m_presetLoader = std::move(loader);
    }
    bool loadPreset(const QString& path, QString* error);
    /// A clean machine for a program load: `preset` if given, else the
    /// model's default preset (Settings), else an All Reset and boot.
    /// `how` says which it was.
    bool cleanStart(const QString& preset, QString* how, QString* error);

    /// What to do once a program is loaded: nothing, type its CALL, or type
    /// its CALL and stop at its entry.
    enum class After { None, Call, StopOnEntry };
    /// Build & Load: loads the program, binds its listing, re-resolves the
    /// breakpoints, then does `after`.
    debug::LoadResult loadProgram(const debug::LoadRequest& request, After after);
    /// Machine reset (all = RAM cleared first) without the boot run: with
    /// `stop` the machine halts before its first instruction (reason
    /// "entry"), else it runs on.
    bool resetMachine(bool allReset, bool stop, QString* error);

    /// The app is running a synchronous load (preset, program): the machine
    /// is being rebuilt or driven directly, so DAP messages wait until it
    /// is done. Nests.
    void setAppBusy(bool busy);

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
    void dispatch(const QJsonObject& message);
    void drainQueue();
    bool m_busy = false;     // handling a message (which may itself load)
    int m_appBusy = 0;       // synchronous loads in progress
    std::vector<QJsonObject> m_queued;
    std::function<bool(const QString&, QString*)> m_presetLoader;
};
