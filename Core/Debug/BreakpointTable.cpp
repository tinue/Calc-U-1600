#include "BreakpointTable.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

namespace debug {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

std::string valueText(int64_t v) {
    char buf[24];
    if (v < 0) std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
    else if (v <= 0xFF) std::snprintf(buf, sizeof buf, "0x%02llX", static_cast<unsigned long long>(v));
    else std::snprintf(buf, sizeof buf, "0x%04llX", static_cast<unsigned long long>(v));
    return buf;
}

BreakpointStatus verifiedStatus(int id, int thread, uint16_t addr) {
    BreakpointStatus st;
    st.id = id;
    st.verified = true;
    st.hasAddress = true;
    st.thread = thread;
    st.addr = addr;
    return st;
}

bool sameStatus(const BreakpointStatus& a, const BreakpointStatus& b) {
    return a.verified == b.verified && a.line == b.line && a.message == b.message && a.addr == b.addr &&
           a.thread == b.thread;
}

} // namespace

HitCondition HitCondition::parse(const std::string& text) {
    HitCondition h;
    std::string t = trim(text);
    if (t.empty()) return h;
    Op op = Eq;
    struct Spelling { const char* text; Op op; };
    static const Spelling kOps[] = {{"==", Eq}, {">=", Ge}, {"<=", Le}, {"!=", Ne}, {">", Gt}, {"<", Lt}, {"%", Mod}, {"=", Eq}};
    for (const Spelling& s : kOps) {
        const size_t n = std::char_traits<char>::length(s.text);
        if (t.compare(0, n, s.text) == 0) { op = s.op; t = trim(t.substr(n)); break; }
    }
    char* end = nullptr;
    const long long n = std::strtoll(t.c_str(), &end, 0);
    if (t.empty() || !end || *end != '\0') {
        h.valid = false; // an unreadable hit condition doesn't hide the breakpoint
        return h;
    }
    h.op = op;
    h.n = n;
    return h;
}

bool HitCondition::met(int hits) const {
    switch (op) {
        case Always: return true;
        case Eq: return hits == n;
        case Ne: return hits != n;
        case Ge: return hits >= n;
        case Le: return hits <= n;
        case Gt: return hits > n;
        case Lt: return hits < n;
        case Mod: return n > 0 && hits % n == 0;
    }
    return true;
}

bool hitConditionMet(const std::string& hitCondition, int hits, bool* valid) {
    const HitCondition h = HitCondition::parse(hitCondition);
    if (valid) *valid = h.valid;
    return h.met(hits);
}

LogTemplate LogTemplate::parse(const std::string& message) {
    LogTemplate t;
    std::string text;
    for (size_t i = 0; i < message.size(); i++) {
        if (message[i] != '{') { text += message[i]; continue; }
        const size_t close = message.find('}', i);
        if (close == std::string::npos) { text += message.substr(i); break; }
        if (!text.empty()) t.m_parts.push_back({text, false, {}});
        text.clear();
        const std::string expr = message.substr(i + 1, close - i - 1);
        t.m_parts.push_back({expr, true, CompiledExpression::compile(expr)});
        i = close;
    }
    if (!text.empty()) t.m_parts.push_back({text, false, {}});
    return t;
}

std::string LogTemplate::render(const ExpressionContext& ctx) const {
    std::string out;
    for (const Part& p : m_parts) {
        if (!p.expression) { out += p.text; continue; }
        const ExpressionResult r = p.compiled.run(ctx);
        out += r.ok ? valueText(r.value) : "{" + r.error + "}";
    }
    return out;
}

std::string interpolateLog(const std::string& message, const ExpressionContext& ctx) {
    return LogTemplate::parse(message).render(ctx);
}

std::shared_ptr<const BreakpointTable::Compiled> BreakpointTable::compile(const BreakpointSpec& spec) {
    auto c = std::make_shared<Compiled>();
    c->hasCondition = !trim(spec.condition).empty();
    if (c->hasCondition) c->condition = CompiledExpression::compile(spec.condition);
    c->hit = HitCondition::parse(spec.hitCondition);
    c->hasLog = !spec.logMessage.empty();
    if (c->hasLog) c->log = LogTemplate::parse(spec.logMessage);
    return c;
}

// ── Setting ───────────────────────────────────────────────────────────────

std::vector<BreakpointStatus> BreakpointTable::resolveSource(Source& s, const SourceMap& map) {
    std::vector<BreakpointStatus> out;
    const std::set<int> ids(s.ids.begin(), s.ids.end());
    m_sourceArmed.erase(std::remove_if(m_sourceArmed.begin(), m_sourceArmed.end(),
                                       [&ids](const Armed& a) { return ids.count(a.id) > 0; }),
                        m_sourceArmed.end());
    for (size_t i = 0; i < s.requests.size(); i++) {
        const SourceRequest& r = s.requests[i];
        int resolved = 0;
        const std::vector<SourceMap::CodeAddress> addrs = map.addressesFor(s.file, r.line, &resolved);
        BreakpointStatus st;
        if (addrs.empty()) {
            st.id = s.ids[i];
            st.line = r.line;
            st.message = "No code at or after this line in a loaded listing";
        } else {
            st = verifiedStatus(s.ids[i], addrs[0].thread, addrs[0].addr);
            st.line = resolved;
            std::string banks;
            const std::shared_ptr<const Compiled> compiled = compile(r);
            for (const auto& a : addrs) {
                Armed armed;
                static_cast<BreakpointSpec&>(armed) = r;
                armed.compiled = compiled;
                armed.id = st.id;
                armed.thread = a.thread;
                armed.addr = a.addr;
                armed.key = a.key;
                m_sourceArmed.push_back(armed);
                if (a.key.any()) banks += (banks.empty() ? "" : "; ") + a.key.describe();
            }
            if (!banks.empty()) st.message = "Only while " + banks;
        }
        out.push_back(st);
    }
    return out;
}

std::vector<BreakpointStatus> BreakpointTable::setSource(const std::string& file, const std::vector<SourceRequest>& requests,
                                                         const SourceMap& map) {
    auto it = std::find_if(m_sources.begin(), m_sources.end(), [&file](const Source& s) { return s.file == file; });
    if (it == m_sources.end()) {
        m_sources.push_back({file, {}, {}});
        it = m_sources.end() - 1;
    }
    // Drop the file's old breakpoints, then set the new ones with fresh ids.
    it->requests.clear();
    resolveSource(*it, map);
    for (int id : it->ids) forgetStatus(id);
    it->requests = requests;
    it->ids.clear();
    for (size_t i = 0; i < requests.size(); i++) it->ids.push_back(m_nextId++);
    std::vector<BreakpointStatus> out = resolveSource(*it, map);
    m_lastStatus.insert(m_lastStatus.end(), out.begin(), out.end());
    return out;
}

std::vector<BreakpointStatus> BreakpointTable::setInstructions(const std::vector<InstructionRequest>& requests) {
    m_instructionArmed.clear();
    std::vector<BreakpointStatus> out;
    for (const InstructionRequest& r : requests) {
        Armed a;
        static_cast<BreakpointSpec&>(a) = r;
        a.compiled = compile(r);
        a.id = m_nextId++;
        a.thread = r.thread;
        a.addr = r.addr;
        m_instructionArmed.push_back(a);
        out.push_back(verifiedStatus(a.id, r.thread, r.addr));
    }
    return out;
}

std::vector<BreakpointStatus> BreakpointTable::resolveFunctions(const SourceMap& map, int thread) {
    m_functionArmed.clear();
    std::vector<BreakpointStatus> out;
    for (const Function& f : m_functions) {
        uint16_t addr = 0;
        if (map.symbolValue(f.request.name, &addr)) {
            Armed a;
            static_cast<BreakpointSpec&>(a) = f.request;
            a.compiled = compile(f.request);
            a.id = f.id;
            a.thread = thread;
            a.addr = addr;
            m_functionArmed.push_back(a);
            out.push_back(verifiedStatus(f.id, thread, addr));
        } else {
            BreakpointStatus st;
            st.id = f.id;
            st.message = "Unknown symbol '" + f.request.name + "'";
            out.push_back(st);
        }
    }
    return out;
}

std::vector<BreakpointStatus> BreakpointTable::setFunctions(const std::vector<FunctionRequest>& requests,
                                                            const SourceMap& map, int thread) {
    for (const Function& f : m_functions) forgetStatus(f.id);
    m_functions.clear();
    for (const FunctionRequest& r : requests) m_functions.push_back({r, m_nextId++});
    std::vector<BreakpointStatus> out = resolveFunctions(map, thread);
    m_lastStatus.insert(m_lastStatus.end(), out.begin(), out.end());
    return out;
}

std::vector<BreakpointStatus> BreakpointTable::setData(const std::vector<DataBreakpointSpec>& requests) {
    m_data.clear();
    std::vector<BreakpointStatus> out;
    for (const DataBreakpointSpec& r : requests) {
        Data d;
        static_cast<DataBreakpointSpec&>(d) = r;
        d.compiled = compile(r);
        d.id = m_nextId++;
        m_data.push_back(d);
        out.push_back(verifiedStatus(d.id, r.thread, r.addr));
    }
    return out;
}

void BreakpointTable::forgetStatus(int id) {
    m_lastStatus.erase(std::remove_if(m_lastStatus.begin(), m_lastStatus.end(),
                                      [id](const BreakpointStatus& s) { return s.id == id; }),
                       m_lastStatus.end());
}

void BreakpointTable::setEntry(int thread, uint16_t addr) {
    m_entryArmed = true;
    m_entryThread = thread;
    m_entryAddr = addr;
}

void BreakpointTable::clear() {
    m_entryArmed = false;
    m_sources.clear();
    m_sourceArmed.clear();
    m_instructionArmed.clear();
    m_functions.clear();
    m_functionArmed.clear();
    m_data.clear();
    m_lastStatus.clear();
}

std::vector<BreakpointStatus> BreakpointTable::reresolve(const SourceMap& map, int functionThread) {
    std::vector<BreakpointStatus> now;
    for (Source& s : m_sources) {
        std::vector<BreakpointStatus> r = resolveSource(s, map);
        now.insert(now.end(), r.begin(), r.end());
    }
    std::vector<BreakpointStatus> f = resolveFunctions(map, functionThread);
    now.insert(now.end(), f.begin(), f.end());
    std::vector<BreakpointStatus> changed;
    for (const BreakpointStatus& st : now) {
        auto old = std::find_if(m_lastStatus.begin(), m_lastStatus.end(),
                                [&st](const BreakpointStatus& o) { return o.id == st.id; });
        if (old == m_lastStatus.end() || !sameStatus(*old, st)) changed.push_back(st);
    }
    m_lastStatus = now;
    return changed;
}

// ── Arming ────────────────────────────────────────────────────────────────

void BreakpointTable::apply(DebugTarget& target) const {
    for (const Thread& t : target.threads()) {
        std::vector<uint16_t> addrs;
        for (const auto* list : {&m_sourceArmed, &m_instructionArmed, &m_functionArmed})
            for (const Armed& a : *list)
                if (a.thread == t.id) addrs.push_back(a.addr);
        if (m_entryArmed && m_entryThread == t.id) addrs.push_back(m_entryAddr);
        target.setBreakpoints(t.id, addrs);
        std::vector<WatchSet::Watch> watches;
        for (const Data& d : m_data) {
            if (d.thread != t.id) continue;
            WatchSet::Watch w;
            w.lo = d.addr;
            w.hi = uint16_t(d.addr + (d.length ? d.length - 1 : 0));
            w.space = d.space;
            w.read = d.access != DataAccess::Write;
            w.write = d.access != DataAccess::Read;
            watches.push_back(w);
        }
        target.setWatches(t.id, watches);
    }
}

// ── Deciding ──────────────────────────────────────────────────────────────

void BreakpointTable::passes(const BreakpointSpec& spec, const Compiled& compiled, int& hits, int thread,
                             DebugTarget& target, const SymbolLookup& symbols, HitDecision* decision, int id) {
    // Only a condition or a log message evaluates anything.
    const ExpressionContext ctx = compiled.hasCondition || compiled.hasLog ? target.expressionContext(thread, symbols)
                                                                           : ExpressionContext{};
    if (compiled.hasCondition) {
        const ExpressionResult r = compiled.condition.run(ctx);
        if (!r.ok) {
            // A broken condition stops, and says why, rather than silently never firing.
            decision->log.push_back("Breakpoint condition '" + spec.condition + "': " + r.error);
            decision->stop = true;
            decision->ids.push_back(id);
            return;
        }
        if (r.value == 0) return;
    }
    hits++;
    if (!compiled.hit.met(hits)) return;
    if (compiled.hasLog) {
        decision->log.push_back(compiled.log.render(ctx));
        return;
    }
    decision->stop = true;
    decision->ids.push_back(id);
}

HitDecision BreakpointTable::onBreakpoint(int thread, uint16_t pc, DebugTarget& target, const SymbolLookup& symbols) {
    HitDecision d;
    if (m_entryArmed && thread == m_entryThread && pc == m_entryAddr) {
        m_entryArmed = false;
        apply(target);
        d.stop = true;
        d.entry = true;
    }
    for (auto* list : {&m_sourceArmed, &m_instructionArmed, &m_functionArmed})
        for (Armed& a : *list) {
            if (a.thread != thread || a.addr != pc) continue;
            if (a.key.any() && !target.bankMatches(thread, a.key, pc)) continue;
            passes(a, *a.compiled, a.hits, thread, target, symbols, &d, a.id);
        }
    return d;
}

HitDecision BreakpointTable::onWatch(int thread, const WatchHit& hit, DebugTarget& target, const SymbolLookup& symbols) {
    HitDecision d;
    for (Data& w : m_data) {
        if (w.thread != thread || w.space != hit.space) continue;
        if (hit.addr < w.addr || hit.addr > uint16_t(w.addr + (w.length ? w.length - 1 : 0))) continue;
        if (hit.write ? w.access == DataAccess::Read : w.access == DataAccess::Write) continue;
        passes(w, *w.compiled, w.hits, thread, target, symbols, &d, w.id);
    }
    return d;
}

} // namespace debug
