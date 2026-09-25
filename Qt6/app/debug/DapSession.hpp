#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

#include "Debug/RunControl.hpp"

class DapServer;
class DebugController;

// One Debug Adapter Protocol session: turns the client's requests into
// calls on the controller's Core debug objects and reports what the run
// control does as DAP events.
//
// Identifiers handed to the client:
//   thread ids   the DebugTarget's (1, and 2 = LH5803 on a PC-1600)
//   frame ids    thread * 1000 + index: 0 = live state, 1..20 = history
//   variables    frame id * 4 + 1 registers / 2 flags / 3 banks
//   memory refs  "<thread>:<hex addr>" plus ":me1" for an LH580x's ME1
class DapSession {
public:
    DapSession(DapServer* server, DebugController* controller);

    void handle(const QJsonObject& request);
    /// Run-control events from the frame slices.
    void onEvents(const std::vector<debug::DebugEvent>& events);
    /// The machine under the session was rebuilt.
    void onMachineReplaced();
    /// Sends a console line to the client.
    void output(const QString& text, const QString& category = QStringLiteral("console"));
    /// Tells the client its session is over (the app is dropping it).
    void terminated() { event(QStringLiteral("terminated")); }

private:
    using Handler = std::function<void(const QJsonObject& args, QJsonObject* body, QString* error)>;

    void respond(const QJsonObject& request, bool success, const QJsonObject& body, const QString& message);
    void event(const QString& name, const QJsonObject& body = {});

    // Requests
    void initialize(const QJsonObject& args, QJsonObject* body, QString* error);
    void attach(const QJsonObject& args, QJsonObject* body, QString* error);
    void configurationDone(const QJsonObject& args, QJsonObject* body, QString* error);
    void disconnect(const QJsonObject& args, QJsonObject* body, QString* error);
    void threads(const QJsonObject& args, QJsonObject* body, QString* error);
    void stackTrace(const QJsonObject& args, QJsonObject* body, QString* error);
    void scopes(const QJsonObject& args, QJsonObject* body, QString* error);
    void variables(const QJsonObject& args, QJsonObject* body, QString* error);
    void setVariable(const QJsonObject& args, QJsonObject* body, QString* error);
    void setBreakpoints(const QJsonObject& args, QJsonObject* body, QString* error);
    void setFunctionBreakpoints(const QJsonObject& args, QJsonObject* body, QString* error);
    void setInstructionBreakpoints(const QJsonObject& args, QJsonObject* body, QString* error);
    void dataBreakpointInfo(const QJsonObject& args, QJsonObject* body, QString* error);
    void setDataBreakpoints(const QJsonObject& args, QJsonObject* body, QString* error);
    void continueRequest(const QJsonObject& args, QJsonObject* body, QString* error);
    void pause(const QJsonObject& args, QJsonObject* body, QString* error);
    void step(const QJsonObject& args, debug::RunControl::StepKind kind, QString* error);
    void disassemble(const QJsonObject& args, QJsonObject* body, QString* error);
    void readMemory(const QJsonObject& args, QJsonObject* body, QString* error);
    void writeMemory(const QJsonObject& args, QJsonObject* body, QString* error);
    void evaluate(const QJsonObject& args, QJsonObject* body, QString* error);
    void restart(const QJsonObject& args, QJsonObject* body, QString* error);
    void customLoad(const QJsonObject& args, QJsonObject* body, QString* error);
    void customReset(const QJsonObject& args, QJsonObject* body, QString* error);

    /// preset, reset, program and listings of an attach configuration.
    bool prepare(const QJsonObject& config, QString* error);
    bool loadProgram(const QJsonObject& descriptor, QJsonObject* body, QString* error);

    // Helpers
    bool ready(QString* error) const;
    int threadOf(const QJsonObject& args) const;
    QJsonObject sourceObject(const QString& path) const;
    QJsonObject statusObject(const debug::BreakpointStatus& st) const;
    QJsonArray registerVariables(int frameId) const;
    QJsonArray flagVariables(int frameId) const;
    QJsonArray bankVariables(int frameId) const;
    std::vector<debug::Register> frameRegisters(int frameId) const;
    debug::ExpressionContext frameContext(int frameId) const;
    QString instructionText(int thread, uint16_t pc) const;
    QString memoryReference(int thread, uint16_t addr, bool me1 = false) const;
    bool parseMemoryReference(const QString& ref, int* thread, uint16_t* addr, bool* me1) const;
    /// A request's memoryReference + offset; false (with `error`) if unreadable.
    bool memoryArgs(const QJsonObject& args, int* thread, uint16_t* addr, bool* me1, QString* error) const;
    /// The thread a configuration's `cpu` names ("z80", "lh5801", "lh5803"); 1 by default.
    int threadForCpu(const QString& cpu) const;
    void sendBreakpointChanges(const std::vector<debug::BreakpointStatus>& changed);

    DapServer* m_server;
    DebugController* m_controller;
    int m_seq = 1;
    bool m_stopOnEntry = false;
    QJsonObject m_attachConfig; // for restart
};
