#include "DapSession.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QTimer>
#include <QJsonDocument>

#include <algorithm>

#include "DapServer.hpp"
#include "DebugController.hpp"
#include "Debug/AttachKeys.hpp"
#include "Debug/Listing/Listing.hpp"
#include "Preset/PresetFile.hpp"

namespace {

constexpr int kHistoryFrames = 20;
constexpr int kVarRegisters = 1, kVarFlags = 2, kVarBanks = 3;

int frameThread(int frameId) { return frameId / 1000; }
int frameIndex(int frameId) { return frameId % 1000; }

QString hex(uint32_t v, int digits) { return QStringLiteral("0x") + QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper(); }

// The condition / hit condition / log message every breakpoint kind shares.
void readSpec(const QJsonObject& o, debug::BreakpointSpec* spec) {
    spec->condition = o.value(QStringLiteral("condition")).toString().toStdString();
    spec->hitCondition = o.value(QStringLiteral("hitCondition")).toString().toStdString();
    spec->logMessage = o.value(QStringLiteral("logMessage")).toString().toStdString();
}

QString registerValue(const debug::Register& r) {
    if (r.bits == 1) return QString::number(r.value);
    return hex(r.value, r.bits == 8 ? 2 : 4);
}

QString displayName(const std::string& name) { return QString::fromStdString(name).toUpper(); }


QString stopReason(debug::DebugEvent::Reason r) {
    switch (r) {
        case debug::DebugEvent::Breakpoint: return QStringLiteral("breakpoint");
        case debug::DebugEvent::DataBreakpoint: return QStringLiteral("data breakpoint");
        case debug::DebugEvent::Step: return QStringLiteral("step");
        case debug::DebugEvent::Pause: return QStringLiteral("pause");
        case debug::DebugEvent::Entry: return QStringLiteral("entry");
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
    else if (command == QLatin1String("restart")) restart(args, &body, &error);
    else if (command == QLatin1String("calcu1600/load")) customLoad(args, &body, &error);
    else if (command == QLatin1String("calcu1600/reset")) customReset(args, &body, &error);
    else if (command == QLatin1String("calcu1600/quit")) {}
    else error = QStringLiteral("Unsupported request '%1'").arg(command);
    respond(request, error.isEmpty(), body, error);

    if (command == QLatin1String("initialize") && error.isEmpty()) event(QStringLiteral("initialized"));
    if (command == QLatin1String("disconnect")) m_server->disconnectClient();
    // Scripted runs (tools/dap_smoke.py) end the app this way: a normal quit,
    // so macOS doesn't take it for a crash and offer to restore windows.
    if (command == QLatin1String("calcu1600/quit")) {
        m_controller->endSession();
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    }
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
        {QStringLiteral("supportsRestartRequest"), true},
    };
}

void DapSession::attach(const QJsonObject& args, QJsonObject*, QString* error) {
    if (!ready(error)) return;
    m_launchArgs = args;
    if (!effectiveConfig(args, &m_attachConfig, error)) return;
    m_stopOnEntry = m_attachConfig.value(QStringLiteral("stopOnEntry")).toBool(false);
    prepare(m_attachConfig, error);
}

namespace {

// A `debug:` block value as the attach configuration's JSON. Bare
// `true`/`false` become JSON booleans; every other scalar stays text.
// Numbers are read later by numberOf(), with the preset's rule.
QJsonValue toJson(const YamlNode& n) {
    switch (n.type) {
        case YamlNode::Type::Map: {
            QJsonObject o;
            for (const auto& kv : n.map) o.insert(QString::fromStdString(kv.first), toJson(kv.second));
            return o;
        }
        case YamlNode::Type::Sequence: {
            QJsonArray a;
            for (const YamlNode& item : n.seq) a.append(toJson(item));
            return a;
        }
        case YamlNode::Type::Scalar: {
            const QString raw = QString::fromStdString(n.scalar);
            std::string text, ignored;
            n.asString(&text, &ignored);
            const QString value = QString::fromStdString(text);
            if (raw != value) return value; // quoted
            if (value == QLatin1String("true")) return true;
            if (value == QLatin1String("false")) return false;
            return value;
        }
        case YamlNode::Type::Null: break;
    }
    return QJsonValue();
}

} // namespace

bool DapSession::effectiveConfig(const QJsonObject& args, QJsonObject* out, QString* error) {
    const QString project = args.value(QStringLiteral("project")).toString();
    if (project.isEmpty()) {
        *out = args;
        return true;
    }
    PresetFile preset;
    std::string parseError;
    const std::string path = debug::absolutePath(project.toStdString());
    if (!parsePresetFile(path, &preset, &parseError)) {
        *error = QStringLiteral("Project %1: %2").arg(project, QString::fromStdString(parseError));
        return false;
    }
    // The project preset sets the machine up; its `debug:` block gives the
    // rest. Keys of the launch configuration win; `program` merges key by key.
    QJsonObject merged = preset.debug.isMap() ? toJson(preset.debug).toObject() : QJsonObject();
    merged.insert(QStringLiteral("preset"), QString::fromStdString(path));
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (it.key() == QLatin1String("program") && merged.value(it.key()).isObject()) {
            QJsonObject program = merged.value(it.key()).toObject();
            const QJsonObject over = it.value().toObject();
            for (auto p = over.begin(); p != over.end(); ++p) program.insert(p.key(), p.value());
            merged.insert(it.key(), program);
        } else {
            merged.insert(it.key(), it.value());
        }
    }
    *out = merged;
    return true;
}

namespace {

// A number of the attach configuration, from a `debug:` block or a launch
// configuration alike: a JSON number, or text with the preset's rule (`&`,
// `0x` or `$` is hex, a bare number decimal).
bool numberOf(const QJsonValue& v, uint32_t max, uint32_t* out) {
    uint32_t n = 0;
    if (v.isDouble()) {
        const double d = v.toDouble();
        if (d < 0 || d != double(uint32_t(d))) return false;
        n = uint32_t(d);
    } else if (!v.isString() || !parseNumber(v.toString().toStdString(), &n)) {
        return false;
    }
    if (n > max) return false;
    *out = n;
    return true;
}

bool parseAddress(const QJsonValue& v, uint32_t* out) { return numberOf(v, 0xFFFF, out); }

// The bank qualifiers `bank`, `me`, `pu`, `pv` (checked by checkAttach); a
// missing one is -1.
debug::BankKey bankKeyOf(const QJsonObject& o) {
    const auto qualifier = [&o](const char* key, uint32_t max) {
        uint32_t n = 0;
        return numberOf(o.value(QLatin1String(key)), max, &n) ? int(n) : -1;
    };
    debug::BankKey k;
    k.bank = qualifier("bank", 7);
    k.me = qualifier("me", 1);
    k.pu = qualifier("pu", 1);
    k.pv = qualifier("pv", 1);
    return k;
}

// Checks the keys of `table` in a launch configuration, a `debug:` block
// or a `calcu1600/load` (Core/Debug/AttachKeys.hpp). Keys the table doesn't
// know pass: a launch configuration also carries the IDE's own.
bool checkAttach(const QJsonObject& o, const debug::attach::Table& table, QString* error) {
    using namespace debug::attach;
    for (const Key& key : table) {
        const QJsonValue v = o.value(QLatin1String(key.name));
        if (v.isUndefined() || v.isNull()) continue;
        std::string why;
        const auto bad = [&](const std::string& what) {
            *error = QStringLiteral("Bad \"%1\": %2").arg(QLatin1String(key.name), QString::fromStdString(what));
            return false;
        };
        switch (key.kind) {
            case Kind::Map:
                if (!v.isObject()) return bad("a block of settings");
                if (!checkAttach(v.toObject(), kProgramKeys, error)) return false;
                break;
            case Kind::FileList:
                if (!v.isArray()) return bad("a list of paths or {path, ...} objects");
                for (const QJsonValue& item : v.toArray()) {
                    if (item.isString()) continue;
                    if (!item.isObject()) return bad("a list of paths or {path, ...} objects");
                    if (!checkAttach(item.toObject(), itemKeys(key), error)) return false;
                }
                break;
            case Kind::PathList:
                if (!v.isArray()) return bad("a list of paths");
                for (const QJsonValue& item : v.toArray())
                    if (!item.isString()) return bad("a list of paths");
                break;
            case Kind::Bool:
                if (!v.isBool()) return bad("true or false");
                break;
            case Kind::Number: {
                uint32_t n = 0;
                if (!numberOf(v, key.max, &n)) {
                    checkScalar(key, std::string(), &why);
                    return bad(why);
                }
                break;
            }
            case Kind::Choice:
                if (!v.isString()) return bad("text");
                if (!checkScalar(key, v.toString().toStdString(), &why)) return bad(why);
                break;
            case Kind::Text: // `entry` may be a number
                if (!v.isString() && !v.isDouble()) return bad("text");
                break;
            case Kind::Path:
                if (!v.isString()) return bad("a path");
                break;
        }
    }
    return true;
}

} // namespace

bool DapSession::loadProgram(const QJsonObject& d, QJsonObject* body, QString* error) {
    if (!checkAttach(d, debug::attach::kProgramKeys, error)) return false;
    debug::LoadRequest req;
    req.bin = debug::absolutePath(d.value(QStringLiteral("bin")).toString().toStdString());
    if (d.value(QStringLiteral("bin")).toString().isEmpty()) {
        *error = QStringLiteral("\"bin\" is missing");
        return false;
    }
    const QString listing = d.value(QStringLiteral("listing")).toString();
    if (!listing.isEmpty()) req.listing = debug::absolutePath(listing.toStdString());
    const QString source = d.value(QStringLiteral("source")).toString();
    if (!source.isEmpty()) req.source = debug::absolutePath(source.toStdString());
    for (const QJsonValue& s : d.value(QStringLiteral("symbols")).toArray())
        req.symbols.push_back(debug::absolutePath(s.toString().toStdString()));
    req.thread = threadForCpu(d.value(QStringLiteral("cpu")).toString());
    req.key = bankKeyOf(d);
    if (d.contains(QStringLiteral("address"))) req.hasAddress = parseAddress(d.value(QStringLiteral("address")), &req.address);
    if (d.contains(QStringLiteral("slot"))) {
        *error = QStringLiteral("\"slot\" was removed: the loader places code by the machine's MODE, the program "
                                "area TITLE selects, and the address. Set MODE / TITLE on the calculator first.");
        return false;
    }
    // A number is the entry; text is a symbol of the listing or an address,
    // which the loader resolves once the listing is read.
    const QJsonValue entry = d.value(QStringLiteral("entry"));
    if (entry.isDouble()) {
        uint32_t e = 0;
        req.hasEntry = parseAddress(entry, &e);
        if (!req.hasEntry) {
            *error = QStringLiteral("Bad \"entry\"");
            return false;
        }
        req.entry = uint16_t(e);
    } else if (entry.isString()) {
        req.entrySymbol = entry.toString().trimmed().toStdString();
    }
    const QString afterText = d.value(QStringLiteral("after")).toString(QStringLiteral("none"));
    const DebugController::After after = afterText == QLatin1String("call")          ? DebugController::After::Call
                                         : afterText == QLatin1String("stopOnEntry") ? DebugController::After::StopOnEntry
                                                                                     : DebugController::After::None;
    // A clean machine first -- the attach configuration's preset, else the
    // model's default preset -- unless the load says otherwise.
    if (d.value(QStringLiteral("cleanStart")).toBool(true)) {
        QString how;
        if (!m_controller->cleanStart(m_attachConfig.value(QStringLiteral("preset")).toString(), &how, error)) {
            *error = QStringLiteral("Clean start failed: %1").arg(*error);
            return false;
        }
        output(QStringLiteral("Clean start: %1").arg(how));
        if (!ready(error)) return false;
    }
    const debug::LoadResult r =
        m_controller->loadProgram(req, after, m_attachConfig.value(QStringLiteral("command")).toString().toStdString());
    if (!r.ok) {
        *error = QStringLiteral("Load failed: %1").arg(QString::fromStdString(r.error));
        return false;
    }
    for (const std::string& w : r.warnings) output(QString::fromStdString(w), QStringLiteral("important"));
    const auto hex4 = [](uint16_t v) { return QStringLiteral("%1").arg(v, 4, 16, QLatin1Char('0')).toUpper(); };
    QString message = QStringLiteral("Loaded %1 at %2-%3")
                          .arg(QFileInfo(QString::fromStdString(req.bin)).fileName(), hex4(r.lo), hex4(r.hi));
    if (!r.callCommand.empty()) message += QStringLiteral("; start with %1").arg(QString::fromStdString(r.callCommand));
    output(message);
    if (body) {
        body->insert(QStringLiteral("start"), memoryReference(req.thread, r.lo));
        body->insert(QStringLiteral("end"), memoryReference(req.thread, r.hi));
        body->insert(QStringLiteral("entry"), memoryReference(req.thread, r.entry));
        body->insert(QStringLiteral("call"), QString::fromStdString(r.callCommand));
    }
    sendBreakpointChanges(m_controller->rebindListings());
    return true;
}

bool DapSession::prepare(const QJsonObject& args, QString* error) {
    // `boot: debug`: the preset only sets the machine up, and the debugger
    // runs the boot, with breakpoints armed (ROM code that runs at power-on).
    if (!checkAttach(args, debug::attach::kBlockKeys, error)) return false;
    const bool bootDebug = args.contains(QStringLiteral("boot"));
    // 1. A preset rebuilds the machine. With a program, the program's clean
    // start (step 4) applies it instead.
    const QString preset = args.value(QStringLiteral("preset")).toString();
    const QJsonObject program = args.value(QStringLiteral("program")).toObject();
    if (bootDebug && (!program.isEmpty() || !args.value(QStringLiteral("command")).toString().isEmpty())) {
        *error = QStringLiteral("\"boot\": \"debug\" runs the boot under the debugger; it takes no \"program\" or "
                                "\"command\"");
        return false;
    }
    if (!preset.isEmpty() && program.isEmpty()) {
        QString err;
        if (!m_controller->loadPreset(preset, &err, /*armOnly=*/bootDebug)) {
            *error = QStringLiteral("Preset failed: %1").arg(err);
            return false;
        }
    }
    if (!ready(error)) return false;
    // 2. Reset (without the boot run: ROM research starts at the vector).
    // `boot: debug` is an All Reset: a power-on of the armed machine.
    const QString reset =
        bootDebug ? QStringLiteral("allReset") : args.value(QStringLiteral("reset")).toString(QStringLiteral("none"));
    if (reset == QLatin1String("reset") || reset == QLatin1String("allReset")) {
        if (!m_controller->resetMachine(reset == QLatin1String("allReset"), /*stop=*/true, error)) return false;
    }
    // 3. Static listings and symbols (ROM listings and the like).
    debug::SourceMap& map = m_controller->sourceMap();
    map.clear();
    // Each entry of `listings` and `symbols` is a path, or an object with
    // `path` and optionally `cpu` and bank qualifiers.
    const auto bindStatic = [this, &map](const QJsonValue& v, bool isListing) {
        const QJsonObject o = v.isString() ? QJsonObject{{QStringLiteral("path"), v.toString()}} : v.toObject();
        const std::string path = o.value(QStringLiteral("path")).toString().toStdString();
        debug::Listing listing;
        std::vector<std::string> warnings;
        const bool ok = isListing ? debug::loadListingWithSymbols(path, o.value(QStringLiteral("source")).toString().toStdString(),
                                                                  {}, &listing, &warnings)
                                  : debug::loadListingWithSymbols({}, {}, {path}, &listing, &warnings);
        for (const std::string& w : warnings)
            output(QString::fromStdString(w), ok ? QStringLiteral("console") : QStringLiteral("important"));
        if (ok) map.addStatic(threadForCpu(o.value(QStringLiteral("cpu")).toString()), std::move(listing), bankKeyOf(o), path);
    };
    for (const QJsonValue& v : args.value(QStringLiteral("listings")).toArray()) bindStatic(v, true);
    for (const QJsonValue& v : args.value(QStringLiteral("symbols")).toArray()) bindStatic(v, false);
    sendBreakpointChanges(m_controller->rebindListings());
    for (const auto& b : map.bindings())
        if (b.stale)
            output(QStringLiteral("%1: %2 of %3 lines don't match memory -- not used for source")
                       .arg(QString::fromStdString(b.name))
                       .arg(b.mismatched)
                       .arg(b.checked),
                   QStringLiteral("important"));
    // 4. The program, bound to its own listing; its start types `command`
    // in place of the CALL. Without a program, `command` is typed here.
    if (!program.isEmpty()) return loadProgram(program, nullptr, error);
    const QString command = args.value(QStringLiteral("command")).toString();
    if (!command.isEmpty()) m_controller->typeCommand(command.toStdString());
    return true;
}

void DapSession::restart(const QJsonObject& args, QJsonObject*, QString* error) {
    if (!ready(error)) return;
    // A restart carries the (possibly edited) attach configuration, and
    // re-reads the project.
    const QJsonObject config = args.value(QStringLiteral("arguments")).toObject();
    if (!config.isEmpty()) m_launchArgs = config;
    if (!effectiveConfig(m_launchArgs, &m_attachConfig, error)) return;
    QJsonObject again = m_attachConfig;
    if (again.value(QStringLiteral("reset")).toString(QStringLiteral("none")) == QLatin1String("none"))
        again.insert(QStringLiteral("reset"), QStringLiteral("reset"));
    m_stopOnEntry = again.value(QStringLiteral("stopOnEntry")).toBool(false);
    if (!prepare(again, error)) return;
    if (m_stopOnEntry) m_controller->runControl()->pause(debug::DebugEvent::Entry);
    else m_controller->runControl()->resume();
}

void DapSession::customLoad(const QJsonObject& args, QJsonObject* body, QString* error) {
    if (!ready(error)) return;
    // The project may have changed since the attach.
    if (!effectiveConfig(m_launchArgs, &m_attachConfig, error)) return;
    // A program given with the request, else the configuration's.
    const QJsonObject program =
        args.contains(QStringLiteral("bin")) ? args : m_attachConfig.value(QStringLiteral("program")).toObject();
    if (!program.isEmpty()) {
        loadProgram(program, body, error);
        return;
    }
    // No program (a ROM extension): the attach set-up again -- the preset
    // plugs the rebuilt ROM in at power-on, the listings are read again,
    // `command` enters the code.
    m_stopOnEntry = m_attachConfig.value(QStringLiteral("stopOnEntry")).toBool(false);
    if (!prepare(m_attachConfig, error)) return;
    if (m_stopOnEntry) m_controller->runControl()->pause(debug::DebugEvent::Entry);
    else m_controller->runControl()->resume();
}

void DapSession::customReset(const QJsonObject& args, QJsonObject*, QString* error) {
    if (!ready(error)) return;
    const bool all = args.value(QStringLiteral("kind")).toString() == QLatin1String("allReset");
    m_controller->resetMachine(all, args.value(QStringLiteral("stop")).toBool(true), error);
}

void DapSession::configurationDone(const QJsonObject&, QJsonObject*, QString* error) {
    if (!ready(error)) return;
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

// Memory references must be plain numbers: VS Code's disassembly view
// parses them (and every instruction address) as one. Thread 1 -- the
// PC-1500's LH5801, the PC-1600's Z-80 -- is the bare 16-bit address; other
// threads sit above it (thread << 16), and an LH580x's ME1 adds 0x100000.
QString DapSession::memoryReference(int thread, uint16_t addr, bool me1) const {
    return hex(uint32_t(addr) | (thread > 1 ? uint32_t(thread) << 16 : 0u) | (me1 ? 0x100000u : 0u), 4);
}

bool DapSession::memoryArgs(const QJsonObject& args, int* thread, uint16_t* addr, bool* me1, QString* error) const {
    if (!parseMemoryReference(args.value(QStringLiteral("memoryReference")).toString(), thread, addr, me1)) {
        *error = QStringLiteral("Bad memory reference");
        return false;
    }
    *addr = uint16_t(*addr + args.value(QStringLiteral("offset")).toInt(0));
    return true;
}

int DapSession::threadForCpu(const QString& cpu) const {
    const QString c = cpu.toLower();
    if (c == QLatin1String("lh5803")) return 2;
    if (const debug::DebugTarget* t = m_controller->target())
        for (const auto& th : t->threads())
            if ((c == QLatin1String("z80") && th.kind == debug::CpuKind::Z80) ||
                (c == QLatin1String("lh5801") && th.kind == debug::CpuKind::LH5801))
                return th.id;
    return 1;
}

bool DapSession::parseMemoryReference(const QString& ref, int* thread, uint16_t* addr, bool* me1) const {
    QString a = ref.trimmed();
    if (!a.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) return false;
    bool ok = false;
    const qulonglong v = a.mid(2).toULongLong(&ok, 16);
    if (!ok || v > 0x1FFFFF) return false;
    *addr = uint16_t(v & 0xFFFF);
    *thread = int((v >> 16) & 0xF);
    if (*thread == 0) *thread = 1;
    *me1 = (v & 0x100000) != 0;
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
                text = QStringLiteral("interrupt at %1").arg(QStringLiteral("%1").arg(pc, 4, 16, QLatin1Char('0')).toUpper());
            } else {
                const debug::SourceMap& map = m_controller->sourceMap();
                const disasm::SymbolFn symbols = [&map, thread](uint16_t a) { return map.symbolAt(thread, a); };
                const disasm::Decoded d = t->decode(thread, h, symbols);
                text = QStringLiteral("after %1  %2")
                           .arg(QStringLiteral("%1").arg(pc, 4, 16, QLatin1Char('0')).toUpper(), QString::fromStdString(d.text));
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
    for (const debug::Register& r : frameRegisters(frameId)) {
        QJsonObject v{{QStringLiteral("name"), displayName(r.name)},
                      {QStringLiteral("value"), registerValue(r)},
                      {QStringLiteral("variablesReference"), 0}};
        if (r.bits == 16) v.insert(QStringLiteral("memoryReference"), memoryReference(frameThread(frameId), uint16_t(r.value)));
        list.append(v);
    }
    const QJsonArray flags = flagVariables(frameId);
    if (!flags.isEmpty()) {
        QStringList summary;
        for (const QJsonValue& f : flags)
            summary << f.toObject().value(QStringLiteral("name")).toString() + f.toObject().value(QStringLiteral("value")).toString();
        list.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("Flags")},
                                {QStringLiteral("value"), summary.join(QLatin1Char(' '))},
                                {QStringLiteral("variablesReference"), frameId * 4 + kVarFlags}});
    }
    // The live frame's bank state, expandable like Flags.
    if (frameIndex(frameId) == 0) {
        QStringList parts;
        for (const QJsonValue& b : bankVariables(frameId))
            parts << QStringLiteral("%1 %2").arg(b.toObject().value(QStringLiteral("name")).toString().left(6),
                                             b.toObject().value(QStringLiteral("value")).toString());
        list.append(QJsonObject{{QStringLiteral("name"), QStringLiteral("Banks")},
                                {QStringLiteral("value"), parts.join(QStringLiteral(", "))},
                                {QStringLiteral("variablesReference"), frameId * 4 + kVarBanks}});
    }
    return list;
}

QJsonArray DapSession::flagVariables(int frameId) const {
    QJsonArray list;
    const debug::CpuKind kind = m_controller->target()->kindOf(frameThread(frameId));
    uint8_t status = 0;
    if (!debug::statusFlags(frameRegisters(frameId), &status)) return list;
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
    for (const debug::BankField& b : m_controller->target()->bankState(frameThread(frameId)))
        list.append(QJsonObject{{QStringLiteral("name"), QString::fromStdString(b.name)},
                                {QStringLiteral("value"), QString::fromStdString(b.value)},
                                {QStringLiteral("variablesReference"), 0}});
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
        readSpec(o, &r);
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
        readSpec(o, &r);
        requests.push_back(r);
    }
    const auto statuses = m_controller->breakpoints().setFunctions(requests, m_controller->sourceMap());
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
        readSpec(o, &r);
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
        readSpec(o, &d);
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
    if (!memoryArgs(args, &thread, &base, &me1, error)) return;
    debug::DebugTarget* t = m_controller->target();
    debug::RunControl* rc = m_controller->runControl();
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
    if (!memoryArgs(args, &thread, &addr, &me1, error)) return;
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
    if (!memoryArgs(args, &thread, &addr, &me1, error)) return;
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
