#include "DapSession.hpp"

#include <QFileInfo>
#include <QJsonDocument>

#include <algorithm>

#include "DapServer.hpp"
#include "DebugController.hpp"
#include "Debug/Disasm/LH5801Disassembler.hpp"
#include "Debug/Disasm/Z80Disassembler.hpp"
#include "Debug/Listing/Listing.hpp"

namespace {

constexpr int kHistoryFrames = 20;
constexpr int kVarRegisters = 1, kVarFlags = 2, kVarBanks = 3;

int frameThread(int frameId) { return frameId / 1000; }
int frameIndex(int frameId) { return frameId % 1000; }

QString hex(uint32_t v, int digits) { return QStringLiteral("0x%1").arg(v, digits, 16, QLatin1Char('0')).toUpper().replace(QStringLiteral("0X"), QStringLiteral("0x")); }

QString registerValue(const debug::Register& r) {
    if (r.bits == 1) return QString::number(r.value);
    return hex(r.value, r.bits == 8 ? 2 : 4);
}

QString displayName(const std::string& name) { return QString::fromStdString(name).toUpper(); }

// The status register of a register list: T on the LH580x, F (low byte of
// AF) on the Z-80.
bool statusRegister(const std::vector<debug::Register>& regs, debug::CpuKind kind, uint8_t* value) {
    for (const auto& r : regs) {
        if (kind == debug::CpuKind::Z80 && r.name == "af") { *value = uint8_t(r.value); return true; }
        if (kind != debug::CpuKind::Z80 && r.name == "t") { *value = uint8_t(r.value); return true; }
    }
    return false;
}

QString stopReason(debug::DebugEvent::Reason r) {
    switch (r) {
        case debug::DebugEvent::Breakpoint: return QStringLiteral("breakpoint");
        case debug::DebugEvent::DataBreakpoint: return QStringLiteral("data breakpoint");
        case debug::DebugEvent::Step: return QStringLiteral("step");
        case debug::DebugEvent::Pause: return QStringLiteral("pause");
        case debug::DebugEvent::Entry: return QStringLiteral("entry");
        case debug::DebugEvent::Goto: return QStringLiteral("goto");
    }
    return QStringLiteral("pause");
}

std::string pathKey(const QString& path) { return debug::absolutePath(path.toStdString()); }

} // namespace

DapSession::DapSession(DapServer* server, DebugController* controller) : m_server(server), m_controller(controller) {}

// ── Transport ─────────────────────────────────────────────────────────────

void DapSession::respond(const QJsonObject& request, bool success, const QJsonObject& body, const QString& message) {
    QJsonObject r{{QStringLiteral("seq"), m_seq++},
                  {QStringLiteral("type"), QStringLiteral("response")},
                  {QStringLiteral("request_seq"), request.value(QStringLiteral("seq"))},
                  {QStringLiteral("command"), request.value(QStringLiteral("command"))},
                  {QStringLiteral("success"), success}};
    if (!body.isEmpty()) r.insert(QStringLiteral("body"), body);
    if (!success) r.insert(QStringLiteral("message"), message);
    m_server->send(r);
}

void DapSession::event(const QString& name, const QJsonObject& body) {
    QJsonObject e{{QStringLiteral("seq"), m_seq++},
                  {QStringLiteral("type"), QStringLiteral("event")},
                  {QStringLiteral("event"), name}};
    if (!body.isEmpty()) e.insert(QStringLiteral("body"), body);
    m_server->send(e);
}

void DapSession::output(const QString& text, const QString& category) {
    event(QStringLiteral("output"), {{QStringLiteral("category"), category},
                                     {QStringLiteral("output"), text.endsWith('\n') ? text : text + '\n'}});
}

void DapSession::handle(const QJsonObject& request) {
    if (request.value(QStringLiteral("type")).toString() != QStringLiteral("request")) return;
    const QString command = request.value(QStringLiteral("command")).toString();
    const QJsonObject args = request.value(QStringLiteral("arguments")).toObject();
    QJsonObject body;
    QString error;
    using Kind = debug::RunControl::StepKind;
    if (command == QLatin1String("initialize")) initialize(args, &body, &error);
    else if (command == QLatin1String("attach") || command == QLatin1String("launch")) attach(args, &body, &error);
    else if (command == QLatin1String("configurationDone")) configurationDone(args, &body, &error);
    else if (command == QLatin1String("disconnect")) disconnect(args, &body, &error);
    else if (command == QLatin1String("threads")) threads(args, &body, &error);
    else if (command == QLatin1String("stackTrace")) stackTrace(args, &body, &error);
    else if (command == QLatin1String("scopes")) scopes(args, &body, &error);
    else if (command == QLatin1String("variables")) variables(args, &body, &error);
    else if (command == QLatin1String("setVariable")) setVariable(args, &body, &error);
    else if (command == QLatin1String("setBreakpoints")) setBreakpoints(args, &body, &error);
    else if (command == QLatin1String("setFunctionBreakpoints")) setFunctionBreakpoints(args, &body, &error);
    else if (command == QLatin1String("setInstructionBreakpoints")) setInstructionBreakpoints(args, &body, &error);
    else if (command == QLatin1String("dataBreakpointInfo")) dataBreakpointInfo(args, &body, &error);
    else if (command == QLatin1String("setDataBreakpoints")) setDataBreakpoints(args, &body, &error);
    else if (command == QLatin1String("setExceptionBreakpoints")) body.insert(QStringLiteral("breakpoints"), QJsonArray());
    else if (command == QLatin1String("continue")) continueRequest(args, &body, &error);
    else if (command == QLatin1String("pause")) pause(args, &body, &error);
    else if (command == QLatin1String("next")) step(args, Kind::Over, &error);
    else if (command == QLatin1String("stepIn")) step(args, Kind::In, &error);
    else if (command == QLatin1String("stepOut")) step(args, Kind::Out, &error);
    else if (command == QLatin1String("disassemble")) disassemble(args, &body, &error);
    else if (command == QLatin1String("readMemory")) readMemory(args, &body, &error);
    else if (command == QLatin1String("writeMemory")) writeMemory(args, &body, &error);
    else if (command == QLatin1String("evaluate")) evaluate(args, &body, &error);
    else error = QStringLiteral("Unsupported request '%1'").arg(command);
    respond(request, error.isEmpty(), body, error);

    if (command == QLatin1String("initialize") && error.isEmpty()) event(QStringLiteral("initialized"));
    if (command == QLatin1String("disconnect")) m_server->disconnectClient();
}

bool DapSession::ready(QString* error) const {
    if (m_controller->target() && m_controller->runControl()) return true;
    *error = QStringLiteral("No machine is running");
    return false;
}

int DapSession::threadOf(const QJsonObject& args) const {
    const int t = args.value(QStringLiteral("threadId")).toInt(1);
    return t >= 1 ? t : 1;
}

// ── Session ───────────────────────────────────────────────────────────────

void DapSession::initialize(const QJsonObject&, QJsonObject* body, QString* error) {
    if (!m_controller->beginSession()) {
        *error = QStringLiteral("No machine is running");
        return;
    }
    *body = QJsonObject{
        {QStringLiteral("supportsConfigurationDoneRequest"), true},
        {QStringLiteral("supportsConditionalBreakpoints"), true},
        {QStringLiteral("supportsHitConditionalBreakpoints"), true},
        {QStringLiteral("supportsLogPoints"), true},
        {QStringLiteral("supportsFunctionBreakpoints"), true},
        {QStringLiteral("supportsInstructionBreakpoints"), true},
        {QStringLiteral("supportsDataBreakpoints"), true},
        {QStringLiteral("supportsDisassembleRequest"), true},
        {QStringLiteral("supportsSteppingGranularity"), true},
        {QStringLiteral("supportsReadMemoryRequest"), true},
        {QStringLiteral("supportsWriteMemoryRequest"), true},
        {QStringLiteral("supportsSetVariable"), true},
        {QStringLiteral("supportsEvaluateForHovers"), true},
        {QStringLiteral("supportsValueFormattingOptions"), false},
    };
}

void DapSession::attach(const QJsonObject& args, QJsonObject*, QString* error) {
    if (!ready(error)) return;
    m_attached = true;
    m_stopOnEntry = args.value(QStringLiteral("stopOnEntry")).toBool(false);
    debug::SourceMap& map = m_controller->sourceMap();
    map.clear();
    const auto threadForCpu = [this](const QString& cpu) {
        const QString c = cpu.toLower();
        if (c == QLatin1String("lh5803")) return 2;
        for (const auto& t : m_controller->target()->threads())
            if ((c == QLatin1String("z80") && t.kind == debug::CpuKind::Z80) ||
                (c == QLatin1String("lh5801") && t.kind == debug::CpuKind::LH5801))
                return t.id;
        return 1;
    };
    const auto keyOf = [](const QJsonObject& o) {
        debug::BankKey k;
        k.bank = o.value(QStringLiteral("bank")).toInt(-1);
        k.me = o.value(QStringLiteral("me")).toInt(-1);
        k.pu = o.value(QStringLiteral("pu")).toInt(-1);
        k.pv = o.value(QStringLiteral("pv")).toInt(-1);
        return k;
    };
    for (const QJsonValue& v : args.value(QStringLiteral("listings")).toArray()) {
        const QJsonObject o = v.isString() ? QJsonObject{{QStringLiteral("path"), v.toString()}} : v.toObject();
        debug::Listing listing;
        std::string err;
        const std::string path = o.value(QStringLiteral("path")).toString().toStdString();
        if (!debug::loadListing(path, &listing, &err, o.value(QStringLiteral("source")).toString().toStdString())) {
            output(QStringLiteral("Listing not loaded: %1").arg(QString::fromStdString(err)), QStringLiteral("important"));
            continue;
        }
        for (const std::string& w : listing.warnings) output(QString::fromStdString(path + ": " + w));
        map.addStatic(threadForCpu(o.value(QStringLiteral("cpu")).toString()), std::move(listing), keyOf(o), path);
    }
    for (const QJsonValue& v : args.value(QStringLiteral("symbols")).toArray()) {
        debug::Listing symbols;
        std::string err;
        const std::string path = v.toString().toStdString();
        if (!debug::loadSymbolFile(path, &symbols, &err)) {
            output(QStringLiteral("Symbols not loaded: %1").arg(QString::fromStdString(err)), QStringLiteral("important"));
            continue;
        }
        map.addStatic(1, std::move(symbols), {}, path);
    }
    sendBreakpointChanges(m_controller->rebindListings());
    for (const auto& b : map.bindings())
        if (b.stale)
            output(QStringLiteral("%1: %2 of %3 lines don't match memory -- not used for source")
                       .arg(QString::fromStdString(b.name))
                       .arg(b.mismatched)
                       .arg(b.checked),
                   QStringLiteral("important"));
}

void DapSession::configurationDone(const QJsonObject&, QJsonObject*, QString* error) {
    if (!ready(error)) return;
    m_configured = true;
    if (m_stopOnEntry) m_controller->runControl()->pause(debug::DebugEvent::Entry);
    else m_controller->runControl()->resume();
}

void DapSession::disconnect(const QJsonObject&, QJsonObject*, QString*) {
    m_controller->endSession();
}

void DapSession::onMachineReplaced() {
    sendBreakpointChanges(m_controller->rebindListings());
    event(QStringLiteral("invalidated"), {{QStringLiteral("areas"), QJsonArray{QStringLiteral("all")}}});
    output(QStringLiteral("The machine was rebuilt; breakpoints re-applied."));
}

void DapSession::onEvents(const std::vector<debug::DebugEvent>& events) {
    for (const debug::DebugEvent& e : events) {
        if (e.kind == debug::DebugEvent::Output) {
            output(QString::fromStdString(e.text));
            continue;
        }
        QJsonObject body{{QStringLiteral("reason"), stopReason(e.reason)},
                         {QStringLiteral("threadId"), e.thread},
                         {QStringLiteral("allThreadsStopped"), true}};
        if (!e.breakpointIds.empty()) {
            QJsonArray ids;
            for (int id : e.breakpointIds) ids.append(id);
            body.insert(QStringLiteral("hitBreakpointIds"), ids);
        }
        event(QStringLiteral("stopped"), body);
    }
}

void DapSession::sendBreakpointChanges(const std::vector<debug::BreakpointStatus>& changed) {
    for (const auto& st : changed)
        event(QStringLiteral("breakpoint"),
              {{QStringLiteral("reason"), QStringLiteral("changed")}, {QStringLiteral("breakpoint"), statusObject(st)}});
}

// ── Threads, frames, variables ────────────────────────────────────────────

void DapSession::threads(const QJsonObject&, QJsonObject* body, QString* error) {
    if (!ready(error)) return;
    debug::DebugTarget* t = m_controller->target();
    QJsonArray list;
    for (const debug::Thread& th : t->threads()) {
        QString name = QString::fromStdString(th.name);
        if (t->threads().size() > 1 && th.id == t->busOwner()) name += QStringLiteral(" [bus]");
        list.append(QJsonObject{{QStringLiteral("id"), th.id}, {QStringLiteral("name"), name}});
    }
    body->insert(QStringLiteral("threads"), list);
}

QString DapSession::instructionText(int thread, uint16_t pc) const {
    const debug::SourceMap& map = m_controller->sourceMap();
    const disasm::Decoded d = m_controller->target()->decode(thread, pc, [&map, thread](uint16_t a) {
        return map.symbolAt(thread, a);
    });
    return QString::fromStdString(d.text);
}

QString DapSession::memoryReference(int thread, uint16_t addr, bool me1) const {
    return QStringLiteral("%1:%2%3").arg(thread).arg(addr, 4, 16, QLatin1Char('0')).toUpper().arg(me1 ? QStringLiteral(":me1") : QString());
}

bool DapSession::parseMemoryReference(const QString& ref, int* thread, uint16_t* addr, bool* me1) const {
    *thread = 1;
    *me1 = false;
    QString a = ref.trimmed();
    const QStringList parts = a.split(':');
    if (parts.size() >= 2) {
        bool ok = false;
        *thread = parts[0].toInt(&ok);
        if (!ok) return false;
        a = parts[1];
        *me1 = parts.size() >= 3 && parts[2].compare(QLatin1String("me1"), Qt::CaseInsensitive) == 0;
    }
    if (a.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) a = a.mid(2);
    bool ok = false;
    const uint v = a.toUInt(&ok, 16);
    if (!ok || v > 0xFFFF) return false;
    *addr = uint16_t(v);
    return true;
}

QJsonObject DapSession::sourceObject(const QString& path) const {
    return {{QStringLiteral("name"), QFileInfo(path).fileName()}, {QStringLiteral("path"), path}};
}

void DapSession::stackTrace(const QJsonObject& args, QJsonObject* body, QString* error) {
    if (!ready(error)) return;
    debug::DebugTarget* t = m_controller->target();
    debug::RunControl* rc = m_controller->runControl();
    const int thread = threadOf(args);
    const int start = args.value(QStringLiteral("startFrame")).toInt(0);
    const int levels = args.value(QStringLiteral("levels")).toInt(0);
    const int history = int(std::min<uint32_t>(t->historySize(thread), kHistoryFrames));
    const int total = 1 + history;
    QJsonArray frames;
    for (int i = start; i < total && (levels <= 0 || frames.size() < levels); i++) {
        QJsonObject f{{QStringLiteral("id"), thread * 1000 + i}, {QStringLiteral("column"), 0}, {QStringLiteral("line"), 0}};
        uint16_t pc;
        if (i == 0) {
            pc = t->pc(thread);
            f.insert(QStringLiteral("name"), QStringLiteral("%1  %2").arg(pc, 4, 16, QLatin1Char('0')).toUpper().arg(instructionText(thread, pc)));
            debug::SourceLocation loc;
            if (rc->locate(thread, pc, &loc)) {
                f.insert(QStringLiteral("source"), sourceObject(QString::fromStdString(loc.file)));
                f.insert(QStringLiteral("line"), loc.line);
            }
        } else {
            const debug::HistoryEntry h = t->history(thread, uint32_t(i - 1));
            pc = h.pc;
            QString text;
            if (h.interrupt) {
                text = QStringLiteral("interrupt at %1").arg(pc, 4, 16, QLatin1Char('0')).toUpper();
            } else {
                const disasm::FetchFn fetch = [&h](uint16_t a) -> uint8_t {
                    const uint16_t k = uint16_t(a - h.pc);
                    return k < h.len ? h.bytes[k] : 0;
                };
                const debug::SourceMap& map = m_controller->sourceMap();
                const disasm::SymbolFn symbols = [&map, thread](uint16_t a) { return map.symbolAt(thread, a); };
                const disasm::Decoded d = t->kindOf(thread) == debug::CpuKind::Z80 ? disasm::decodeZ80(h.pc, fetch, symbols)
                                                                                 : disasm::decodeLH5801(h.pc, fetch, symbols);
                text = QStringLiteral("after %1  %2").arg(pc, 4, 16, QLatin1Char('0')).toUpper().arg(QString::fromStdString(d.text));
                debug::SourceLocation loc;
                if (m_controller->sourceMap().lookup(thread, pc, rc->bankMatch(), &loc)) {
                    f.insert(QStringLiteral("source"), sourceObject(QString::fromStdString(loc.file)));
                    f.insert(QStringLiteral("line"), loc.line);
                }
            }
            f.insert(QStringLiteral("name"), text);
            f.insert(QStringLiteral("presentationHint"), QStringLiteral("subtle"));
        }
        f.insert(QStringLiteral("instructionPointerReference"), memoryReference(thread, pc));
        frames.append(f);
    }
    body->insert(QStringLiteral("stackFrames"), frames);
    body->insert(QStringLiteral("totalFrames"), total);
}

void DapSession::scopes(const QJsonObject& args, QJsonObject* body, QString* error) {
    if (!ready(error)) return;
    const int frameId = args.value(QStringLiteral("frameId")).toInt();
    QJsonArray list;
    list.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("Registers")},
                            {QStringLiteral("presentationHint"), QStringLiteral("registers")},
                            {QStringLiteral("variablesReference"), frameId * 4 + kVarRegisters},
                            {QStringLiteral("expensive"), false}});
    if (frameIndex(frameId) == 0)
        list.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("Banks")},
                                {QStringLiteral("variablesReference"), frameId * 4 + kVarBanks},
                                {QStringLiteral("expensive"), false}});
    body->insert(QStringLiteral("scopes"), list);
}

std::vector<debug::Register> DapSession::frameRegisters(int frameId) const {
    debug::DebugTarget* t = m_controller->target();
    const int thread = frameThread(frameId), index = frameIndex(frameId);
    if (index == 0) return t->registers(thread);
    if (uint32_t(index - 1) >= t->historySize(thread)) return {};
    return t->history(thread, uint32_t(index - 1)).registers;
}

QJsonArray DapSession::registerVariables(int frameId) const {
    QJsonArray list;
    const debug::CpuKind kind = m_controller->target()->kindOf(frameThread(frameId));
    for (const debug::Register& r : frameRegisters(frameId)) {
        QJsonObject v{{QStringLiteral("name"), displayName(r.name)},
                      {QStringLiteral("value"), registerValue(r)},
                      {QStringLiteral("variablesReference"), 0}};
        if (r.bits == 16) v.insert(QStringLiteral("memoryReference"), memoryReference(frameThread(frameId), uint16_t(r.value)));
        list.append(v);
    }
    uint8_t status = 0;
    if (statusRegister(frameRegisters(frameId), kind, &status)) {
        QString summary;
        const auto& names = debug::flagNames(kind);
        for (size_t i = names.size(); i-- > 0;)
            if (!names[i].empty()) summary += QStringLiteral("%1%2 ").arg(QString::fromStdString(names[i]).toUpper()).arg((status >> i) & 1);
        list.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("Flags")},
                                {QStringLiteral("value"), summary.trimmed()},
                                {QStringLiteral("variablesReference"), frameId * 4 + kVarFlags}});
    }
    return list;
}

QJsonArray DapSession::flagVariables(int frameId) const {
    QJsonArray list;
    const debug::CpuKind kind = m_controller->target()->kindOf(frameThread(frameId));
    uint8_t status = 0;
    if (!statusRegister(frameRegisters(frameId), kind, &status)) return list;
    const auto& names = debug::flagNames(kind);
    for (size_t i = names.size(); i-- > 0;)
        if (!names[i].empty())
            list.append(QJsonObject{{QStringLiteral("name"), QString::fromStdString(names[i]).toUpper()},
                                    {QStringLiteral("value"), QString::number((status >> i) & 1)},
                                    {QStringLiteral("variablesReference"), 0}});
    return list;
}

QJsonArray DapSession::bankVariables(int frameId) const {
    QJsonArray list;
    debug::DebugTarget* t = m_controller->target();
    const int thread = frameThread(frameId);
    if (t->kindOf(thread) == debug::CpuKind::Z80) {
        static const char* const kPages[4] = {"Page A (0000-3FFF)", "Page B (4000-7FFF)", "Page C (8000-BFFF)",
                                              "Page D (C000-FFFF)"};
        for (int p = 0; p < 4; p++)
            list.append(QJsonObject{{QStringLiteral("name"), QString::fromLatin1(kPages[p])},
                                    {QStringLiteral("value"), QStringLiteral("bank %1").arg(t->bankAt(thread, uint16_t(p << 14)))},
                                    {QStringLiteral("variablesReference"), 0}});
    } else {
        list.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("PU")},
                                {QStringLiteral("value"), QString::number(t->pu(thread) ? 1 : 0)},
                                {QStringLiteral("variablesReference"), 0}});
        list.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("PV")},
                                {QStringLiteral("value"), QString::number(t->pv(thread) ? 1 : 0)},
                                {QStringLiteral("variablesReference"), 0}});
    }
    return list;
}

void DapSession::variables(const QJsonObject& args, QJsonObject* body, QString* error) {
    if (!ready(error)) return;
    const int ref = args.value(QStringLiteral("variablesReference")).toInt();
    const int frameId = ref / 4;
    QJsonArray list;
    switch (ref % 4) {
        case kVarRegisters: list = registerVariables(frameId); break;
        case kVarFlags: list = flagVariables(frameId); break;
        case kVarBanks: list = bankVariables(frameId); break;
        default: break;
    }
    body->insert(QStringLiteral("variables"), list);
}

debug::ExpressionContext DapSession::frameContext(int frameId) const {
    debug::DebugTarget* t = m_controller->target();
    const int thread = std::max(1, frameThread(frameId));
    debug::ExpressionContext ctx = t->expressionContext(thread, m_controller->runControl()->symbols());
    if (frameIndex(frameId) > 0) {
        // A history frame: its own registers first.
        const std::vector<debug::Register> regs = frameRegisters(frameId);
        const auto fallback = ctx.lookup;
        ctx.lookup = [regs, fallback](const std::string& name, int64_t* value) {
            for (const auto& r : regs)
                if (r.name == name) { *value = r.value; return true; }
            return fallback(name, value);
        };
    }
    return ctx;
}

void DapSession::setVariable(const QJsonObject& args, QJsonObject* body, QString* error) {
    if (!ready(error)) return;
    const int ref = args.value(QStringLiteral("variablesReference")).toInt();
    const int frameId = ref / 4;
    if (frameIndex(frameId) != 0) {
        *error = QStringLiteral("Only the live frame's registers can be changed");
        return;
    }
    const int thread = frameThread(frameId);
    const debug::ExpressionResult r =
        debug::evaluate(args.value(QStringLiteral("value")).toString().toStdString(), frameContext(frameId));
    if (!r.ok) {
        *error = QString::fromStdString(r.error);
        return;
    }
    std::string name = args.value(QStringLiteral("name")).toString().toLower().toStdString();
    if (ref % 4 == kVarFlags) name += "f";
    if (ref % 4 == kVarBanks || !m_controller->target()->writeRegister(thread, name, uint32_t(r.value))) {
        *error = QStringLiteral("Can't set %1").arg(args.value(QStringLiteral("name")).toString());
        return;
    }
    // Show the value as the register holds it now (a flag as 0/1).
    uint32_t now = 0;
    m_controller->target()->readRegister(thread, name, &now);
    debug::Register reg{name, now, uint8_t(ref % 4 == kVarFlags ? 1 : 8)};
    for (const auto& x : m_controller->target()->registers(thread))
        if (x.name == name) reg.bits = x.bits;
    body->insert(QStringLiteral("value"), registerValue(reg));
}

// ── Breakpoints ───────────────────────────────────────────────────────────

QJsonObject DapSession::statusObject(const debug::BreakpointStatus& st) const {
    QJsonObject o{{QStringLiteral("id"), st.id}, {QStringLiteral("verified"), st.verified}};
    if (st.line > 0) o.insert(QStringLiteral("line"), st.line);
    if (!st.message.empty()) o.insert(QStringLiteral("message"), QString::fromStdString(st.message));
    if (st.hasAddress) o.insert(QStringLiteral("instructionReference"), memoryReference(st.thread, st.addr));
    return o;
}

void DapSession::setBreakpoints(const QJsonObject& args, QJsonObject* body, QString* error) {
    if (!ready(error)) return;
    const QString path = args.value(QStringLiteral("source")).toObject().value(QStringLiteral("path")).toString();
    std::vector<debug::BreakpointTable::SourceRequest> requests;
    for (const QJsonValue& v : args.value(QStringLiteral("breakpoints")).toArray()) {
        const QJsonObject o = v.toObject();
        debug::BreakpointTable::SourceRequest r;
        r.line = o.value(QStringLiteral("line")).toInt();
        r.condition = o.value(QStringLiteral("condition")).toString().toStdString();
        r.hitCondition = o.value(QStringLiteral("hitCondition")).toString().toStdString();
        r.logMessage = o.value(QStringLiteral("logMessage")).toString().toStdString();
        requests.push_back(r);
    }
    const auto statuses = m_controller->breakpoints().setSource(pathKey(path), requests, m_controller->sourceMap());
    m_controller->breakpoints().apply(*m_controller->target());
    QJsonArray list;
    for (const auto& st : statuses) {
        QJsonObject o = statusObject(st);
        o.insert(QStringLiteral("source"), sourceObject(path));
        list.append(o);
    }
    body->insert(QStringLiteral("breakpoints"), list);
}

void DapSession::setFunctionBreakpoints(const QJsonObject& args, QJsonObject* body, QString* error) {
    if (!ready(error)) return;
    std::vector<debug::BreakpointTable::FunctionRequest> requests;
    for (const QJsonValue& v : args.value(QStringLiteral("breakpoints")).toArray()) {
        const QJsonObject o = v.toObject();
        debug::BreakpointTable::FunctionRequest r;
        r.name = o.value(QStringLiteral("name")).toString().toStdString();
        r.condition = o.value(QStringLiteral("condition")).toString().toStdString();
        r.hitCondition = o.value(QStringLiteral("hitCondition")).toString().toStdString();
        requests.push_back(r);
    }
    const auto statuses = m_controller->breakpoints().setFunctions(requests, m_controller->sourceMap(), 1);
    m_controller->breakpoints().apply(*m_controller->target());
    QJsonArray list;
    for (const auto& st : statuses) list.append(statusObject(st));
    body->insert(QStringLiteral("breakpoints"), list);
}

void DapSession::setInstructionBreakpoints(const QJsonObject& args, QJsonObject* body, QString* error) {
    if (!ready(error)) return;
    std::vector<debug::BreakpointTable::InstructionRequest> requests;
    std::vector<bool> valid;
    for (const QJsonValue& v : args.value(QStringLiteral("breakpoints")).toArray()) {
        const QJsonObject o = v.toObject();
        debug::BreakpointTable::InstructionRequest r;
        int thread = 1;
        uint16_t addr = 0;
        bool me1 = false;
        const bool ok = parseMemoryReference(o.value(QStringLiteral("instructionReference")).toString(), &thread, &addr, &me1);
        valid.push_back(ok);
        if (!ok) continue;
        r.thread = thread;
        r.addr = uint16_t(addr + o.value(QStringLiteral("offset")).toInt(0));
        r.condition = o.value(QStringLiteral("condition")).toString().toStdString();
        r.hitCondition = o.value(QStringLiteral("hitCondition")).toString().toStdString();
        requests.push_back(r);
    }
    const auto statuses = m_controller->breakpoints().setInstructions(requests);
    m_controller->breakpoints().apply(*m_controller->target());
    QJsonArray list;
    size_t next = 0;
    for (bool ok : valid) {
        if (ok) list.append(statusObject(statuses[next++]));
        else list.append(QJsonObject{{QStringLiteral("verified"), false},
                                     {QStringLiteral("message"), QStringLiteral("Unknown instruction reference")}});
    }
    body->insert(QStringLiteral("breakpoints"), list);
}

void DapSession::dataBreakpointInfo(const QJsonObject& args, QJsonObject* body, QString* error) {
    if (!ready(error)) return;
    const QString name = args.value(QStringLiteral("name")).toString();
    const int ref = args.value(QStringLiteral("variablesReference")).toInt(0);
    if (ref != 0) {
        body->insert(QStringLiteral("dataId"), QJsonValue());
        body->insert(QStringLiteral("description"), QStringLiteral("Registers can't be watched; use a conditional breakpoint"));
        return;
    }
    // An address, symbol or expression; with asAddress the name is the address.
    int thread = 1;
    uint16_t addr = 0;
    bool me1 = false;
    if (!parseMemoryReference(name, &thread, &addr, &me1)) {
        const int frameId = args.value(QStringLiteral("frameId")).toInt(1000);
        const debug::ExpressionResult r = debug::evaluate(name.toStdString(), frameContext(frameId));
        if (!r.ok || r.value < 0 || r.value > 0xFFFF) {
            body->insert(QStringLiteral("dataId"), QJsonValue());
            body->insert(QStringLiteral("description"), QStringLiteral("Not a memory address"));
            return;
        }
        thread = std::max(1, frameThread(frameId));
        addr = uint16_t(r.value);
    }
    const int length = std::max(1, args.value(QStringLiteral("bytes")).toInt(1));
    body->insert(QStringLiteral("dataId"), QStringLiteral("%1:%2:%3:%4").arg(thread).arg(me1 ? 1 : 0).arg(addr).arg(length));
    body->insert(QStringLiteral("description"),
                 QStringLiteral("%1%2 (%3 byte%4)").arg(me1 ? QStringLiteral("#") : QString()).arg(hex(addr, 4)).arg(length).arg(length > 1 ? "s" : ""));
    body->insert(QStringLiteral("accessTypes"), QJsonArray{QStringLiteral("read"), QStringLiteral("write"), QStringLiteral("readWrite")});
    body->insert(QStringLiteral("canPersist"), true);
}

void DapSession::setDataBreakpoints(const QJsonObject& args, QJsonObject* body, QString* error) {
    if (!ready(error)) return;
    std::vector<debug::DataBreakpointSpec> requests;
    for (const QJsonValue& v : args.value(QStringLiteral("breakpoints")).toArray()) {
        const QJsonObject o = v.toObject();
        const QStringList id = o.value(QStringLiteral("dataId")).toString().split(':');
        if (id.size() != 4) continue;
        debug::DataBreakpointSpec d;
        d.thread = id[0].toInt();
        d.space = id[1].toInt() ? debug::kSpaceME1 : debug::kSpaceMain;
        d.addr = uint16_t(id[2].toUInt());
        d.length = uint16_t(std::max(1, id[3].toInt()));
        const QString access = o.value(QStringLiteral("accessType")).toString(QStringLiteral("write"));
        d.access = access == QLatin1String("read") ? debug::DataAccess::Read
                   : access == QLatin1String("readWrite") ? debug::DataAccess::ReadWrite
                                                          : debug::DataAccess::Write;
        d.condition = o.value(QStringLiteral("condition")).toString().toStdString();
        d.hitCondition = o.value(QStringLiteral("hitCondition")).toString().toStdString();
        requests.push_back(d);
    }
    const auto statuses = m_controller->breakpoints().setData(requests);
    m_controller->breakpoints().apply(*m_controller->target());
    QJsonArray list;
    for (const auto& st : statuses) list.append(statusObject(st));
    body->insert(QStringLiteral("breakpoints"), list);
}

// ── Execution ─────────────────────────────────────────────────────────────

void DapSession::continueRequest(const QJsonObject&, QJsonObject* body, QString* error) {
    if (!ready(error)) return;
    m_controller->runControl()->resume();
    body->insert(QStringLiteral("allThreadsContinued"), true);
}

void DapSession::pause(const QJsonObject&, QJsonObject*, QString* error) {
    if (!ready(error)) return;
    m_controller->runControl()->pause();
}

void DapSession::step(const QJsonObject& args, debug::RunControl::StepKind kind, QString* error) {
    if (!ready(error)) return;
    const bool instruction = args.value(QStringLiteral("granularity")).toString() == QLatin1String("instruction");
    m_controller->runControl()->step(threadOf(args), kind, !instruction);
}

// ── Code and memory ───────────────────────────────────────────────────────

void DapSession::disassemble(const QJsonObject& args, QJsonObject* body, QString* error) {
    if (!ready(error)) return;
    int thread = 1;
    uint16_t base = 0;
    bool me1 = false;
    if (!parseMemoryReference(args.value(QStringLiteral("memoryReference")).toString(), &thread, &base, &me1)) {
        *error = QStringLiteral("Bad memory reference");
        return;
    }
    debug::DebugTarget* t = m_controller->target();
    debug::RunControl* rc = m_controller->runControl();
    base = uint16_t(base + args.value(QStringLiteral("offset")).toInt(0));
    const int instructionOffset = args.value(QStringLiteral("instructionOffset")).toInt(0);
    const int count = args.value(QStringLiteral("instructionCount")).toInt(0);

    // Instruction boundaries before `base`: decode forward from a little
    // earlier and keep a start that lands exactly on `base`.
    uint16_t start = base;
    if (instructionOffset < 0) {
        const int back = -instructionOffset;
        std::vector<uint16_t> best;
        for (int slack = 0; slack < 6 && best.size() < size_t(back); slack++) {
            std::vector<uint16_t> starts;
            uint16_t a = uint16_t(base - back * 4 - slack);
            while (a != base && uint16_t(base - a) <= uint16_t(back * 4 + slack)) {
                starts.push_back(a);
                a = uint16_t(a + t->decode(thread, a).len);
            }
            if (a == base && starts.size() > best.size()) best = starts;
        }
        // Too little decodable code before `base`: start as early as we could.
        if (best.size() >= size_t(back)) start = best[best.size() - size_t(back)];
        else start = best.empty() ? base : best.front();
    } else {
        for (int i = 0; i < instructionOffset; i++) start = uint16_t(start + t->decode(thread, start).len);
    }

    const debug::SourceMap& map = m_controller->sourceMap();
    QJsonArray list;
    QString lastFile;
    uint16_t a = start;
    for (int i = 0; i < count; i++) {
        const disasm::Decoded d = t->decode(thread, a, [&map, thread](uint16_t x) { return map.symbolAt(thread, x); });
        QStringList bytes;
        for (int k = 0; k < d.len; k++) {
            uint8_t b = 0;
            t->peek(thread, debug::kSpaceMain, uint16_t(a + k), &b);
            bytes << QStringLiteral("%1").arg(b, 2, 16, QLatin1Char('0')).toUpper();
        }
        QJsonObject ins{{QStringLiteral("address"), memoryReference(thread, a)},
                        {QStringLiteral("instructionBytes"), bytes.join(' ')},
                        {QStringLiteral("instruction"), QString::fromStdString(d.text)}};
        const std::string label = map.symbolAt(thread, a);
        if (!label.empty()) ins.insert(QStringLiteral("symbol"), QString::fromStdString(label));
        debug::SourceLocation loc;
        if (rc->locate(thread, a, &loc)) {
            const QString file = QString::fromStdString(loc.file);
            if (file != lastFile) ins.insert(QStringLiteral("location"), sourceObject(file));
            lastFile = file;
            ins.insert(QStringLiteral("line"), loc.line);
        }
        list.append(ins);
        a = uint16_t(a + d.len);
    }
    body->insert(QStringLiteral("instructions"), list);
}

void DapSession::readMemory(const QJsonObject& args, QJsonObject* body, QString* error) {
    if (!ready(error)) return;
    int thread = 1;
    uint16_t addr = 0;
    bool me1 = false;
    if (!parseMemoryReference(args.value(QStringLiteral("memoryReference")).toString(), &thread, &addr, &me1)) {
        *error = QStringLiteral("Bad memory reference");
        return;
    }
    addr = uint16_t(addr + args.value(QStringLiteral("offset")).toInt(0));
    const int count = std::min(args.value(QStringLiteral("count")).toInt(0), 0x10000);
    QByteArray data;
    int unreadable = 0;
    for (int i = 0; i < count; i++) {
        uint8_t b = 0;
        if (!m_controller->target()->peek(thread, me1 ? debug::kSpaceME1 : debug::kSpaceMain, uint16_t(addr + i), &b)) {
            if (data.isEmpty()) { unreadable++; continue; } // leading unreadable bytes are reported, not sent
            break;
        }
        data.append(char(b));
    }
    body->insert(QStringLiteral("address"), memoryReference(thread, uint16_t(addr + unreadable), me1));
    body->insert(QStringLiteral("data"), QString::fromLatin1(data.toBase64()));
    if (unreadable) body->insert(QStringLiteral("unreadableBytes"), unreadable);
}

void DapSession::writeMemory(const QJsonObject& args, QJsonObject* body, QString* error) {
    if (!ready(error)) return;
    int thread = 1;
    uint16_t addr = 0;
    bool me1 = false;
    if (!parseMemoryReference(args.value(QStringLiteral("memoryReference")).toString(), &thread, &addr, &me1)) {
        *error = QStringLiteral("Bad memory reference");
        return;
    }
    addr = uint16_t(addr + args.value(QStringLiteral("offset")).toInt(0));
    const QByteArray data = QByteArray::fromBase64(args.value(QStringLiteral("data")).toString().toLatin1());
    int written = 0;
    for (char c : data) {
        if (!m_controller->target()->poke(thread, me1 ? debug::kSpaceME1 : debug::kSpaceMain, uint16_t(addr + written), uint8_t(c)))
            break;
        written++;
    }
    body->insert(QStringLiteral("bytesWritten"), written);
    if (written < data.size() && written == 0) *error = QStringLiteral("Memory at %1 isn't writable").arg(hex(addr, 4));
    else event(QStringLiteral("memory"), {{QStringLiteral("memoryReference"), memoryReference(thread, addr, me1)},
                                          {QStringLiteral("offset"), 0},
                                          {QStringLiteral("count"), written}});
}

void DapSession::evaluate(const QJsonObject& args, QJsonObject* body, QString* error) {
    if (!ready(error)) return;
    const int frameId = args.value(QStringLiteral("frameId")).toInt(m_controller->target()->busOwner() * 1000);
    const debug::ExpressionResult r =
        debug::evaluate(args.value(QStringLiteral("expression")).toString().toStdString(), frameContext(frameId));
    if (!r.ok) {
        *error = QString::fromStdString(r.error);
        return;
    }
    QString text;
    if (r.value >= 0 && r.value <= 0xFFFF)
        text = QStringLiteral("%1 (%2)").arg(hex(uint32_t(r.value), r.value > 0xFF ? 4 : 2)).arg(r.value);
    else
        text = QString::number(r.value);
    body->insert(QStringLiteral("result"), text);
    body->insert(QStringLiteral("variablesReference"), 0);
    if (r.value >= 0 && r.value <= 0xFFFF)
        body->insert(QStringLiteral("memoryReference"), memoryReference(std::max(1, frameThread(frameId)), uint16_t(r.value)));
}
