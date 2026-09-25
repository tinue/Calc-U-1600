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

bool sameStatus(const BreakpointStatus& a, const BreakpointStatus& b) {
    return a.verified == b.verified && a.line == b.line && a.message == b.message && a.addr == b.addr &&
           a.thread == b.thread;
}

} // namespace

bool hitConditionMet(const std::string& hitCondition, int hits, bool* valid) {
    std::string t = trim(hitCondition);
    if (valid) *valid = true;
    if (t.empty()) return true;
    std::string op = "==";
    for (const char* candidate : {"==", ">=", "<=", "!=", ">", "<", "%", "="}) {
        const size_t n = std::char_traits<char>::length(candidate);
        if (t.compare(0, n, candidate) == 0) { op = candidate; t = trim(t.substr(n)); break; }
    }
    char* end = nullptr;
    const long long n = std::strtoll(t.c_str(), &end, 0);
    if (t.empty() || !end || *end != '\0') {
        if (valid) *valid = false;
        return true; // an unreadable hit condition doesn't hide the breakpoint
    }
    if (op == "==" || op == "=") return hits == n;
    if (op == ">=") return hits >= n;
    if (op == "<=") return hits <= n;
    if (op == "!=") return hits != n;
    if (op == ">") return hits > n;
    if (op == "<") return hits < n;
    return n > 0 && hits % n == 0; // "%"
}

std::string interpolateLog(const std::string& message, const ExpressionContext& ctx) {
    std::string out;
    for (size_t i = 0; i < message.size(); i++) {
        if (message[i] != '{') { out += message[i]; continue; }
        const size_t close = message.find('}', i);
        if (close == std::string::npos) { out += message.substr(i); break; }
        const ExpressionResult r = evaluate(message.substr(i + 1, close - i - 1), ctx);
        out += r.ok ? valueText(r.value) : "{" + r.error + "}";
        i = close;
    }
    return out;
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
        BreakpointStatus st;
        st.id = s.ids[i];
        int resolved = 0;
        const std::vector<SourceMap::CodeAddress> addrs = map.addressesFor(s.file, r.line, &resolved);
        if (addrs.empty()) {
            st.line = r.line;
            st.message = "No code at or after this line in a loaded listing";
        } else {
            st.verified = true;
            st.line = resolved;
            st.hasAddress = true;
            st.thread = addrs[0].thread;
            st.addr = addrs[0].addr;
            std::string banks;
            for (const auto& a : addrs) {
                Armed armed;
                static_cast<BreakpointSpec&>(armed) = r;
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
    for (int id : it->ids)
        m_lastStatus.erase(std::remove_if(m_lastStatus.begin(), m_lastStatus.end(),
                                          [id](const BreakpointStatus& s) { return s.id == id; }),
                           m_lastStatus.end());
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
        a.id = m_nextId++;
        a.thread = r.thread;
        a.addr = r.addr;
        m_instructionArmed.push_back(a);
        BreakpointStatus st;
        st.id = a.id;
        st.verified = true;
        st.hasAddress = true;
        st.thread = r.thread;
        st.addr = r.addr;
        out.push_back(st);
    }
    return out;
}

std::vector<BreakpointStatus> BreakpointTable::resolveFunctions(const SourceMap& map, int thread) {
    m_functionArmed.clear();
    std::vector<BreakpointStatus> out;
    for (const Function& f : m_functions) {
        BreakpointStatus st;
        st.id = f.id;
        uint16_t addr = 0;
        if (map.symbolValue(f.request.name, &addr)) {
            Armed a;
            static_cast<BreakpointSpec&>(a) = f.request;
            a.id = f.id;
            a.thread = thread;
            a.addr = addr;
            m_functionArmed.push_back(a);
            st.verified = true;
            st.hasAddress = true;
            st.thread = thread;
            st.addr = addr;
        } else {
            st.message = "Unknown symbol '" + f.request.name + "'";
        }
        out.push_back(st);
    }
    return out;
}

std::vector<BreakpointStatus> BreakpointTable::setFunctions(const std::vector<FunctionRequest>& requests,
                                                            const SourceMap& map, int thread) {
    for (const Function& f : m_functions)
        m_lastStatus.erase(std::remove_if(m_lastStatus.begin(), m_lastStatus.end(),
                                          [&f](const BreakpointStatus& s) { return s.id == f.id; }),
                           m_lastStatus.end());
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
        d.id = m_nextId++;
        m_data.push_back(d);
        BreakpointStatus st;
        st.id = d.id;
        st.verified = true;
        st.thread = r.thread;
        st.addr = r.addr;
        st.hasAddress = true;
        out.push_back(st);
    }
    return out;
}

void BreakpointTable::clear() {
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

bool BreakpointTable::passes(BreakpointSpec& spec, int& hits, int thread, DebugTarget& target,
                             const SymbolLookup& symbols, HitDecision* decision, int id) {
    const ExpressionContext ctx = target.expressionContext(thread, symbols);
    if (!trim(spec.condition).empty()) {
        const ExpressionResult r = evaluate(spec.condition, ctx);
        if (!r.ok) {
            // A broken condition stops, and says why, rather than silently never firing.
            decision->log.push_back("Breakpoint condition '" + spec.condition + "': " + r.error);
            decision->stop = true;
            decision->ids.push_back(id);
            return false;
        }
        if (r.value == 0) return false;
    }
    hits++;
    if (!hitConditionMet(spec.hitCondition, hits)) return false;
    if (!spec.logMessage.empty()) {
        decision->log.push_back(interpolateLog(spec.logMessage, ctx));
        return false;
    }
    decision->stop = true;
    decision->ids.push_back(id);
    return true;
}

HitDecision BreakpointTable::onBreakpoint(int thread, uint16_t pc, DebugTarget& target, const SymbolLookup& symbols) {
    HitDecision d;
    for (auto* list : {&m_sourceArmed, &m_instructionArmed, &m_functionArmed})
        for (Armed& a : *list) {
            if (a.thread != thread || a.addr != pc) continue;
            if (a.key.any() && !target.bankMatches(thread, a.key, pc)) continue;
            passes(a, a.hits, thread, target, symbols, &d, a.id);
        }
    return d;
}

HitDecision BreakpointTable::onWatch(int thread, const WatchHit& hit, DebugTarget& target, const SymbolLookup& symbols) {
    HitDecision d;
    for (Data& w : m_data) {
        if (w.thread != thread || w.space != hit.space) continue;
        if (hit.addr < w.addr || hit.addr > uint16_t(w.addr + (w.length ? w.length - 1 : 0))) continue;
        if (hit.write ? w.access == DataAccess::Read : w.access == DataAccess::Write) continue;
        passes(w, w.hits, thread, target, symbols, &d, w.id);
    }
    return d;
}

bool BreakpointTable::hasBreakpointAt(int thread, uint16_t pc) const {
    for (const auto* list : {&m_sourceArmed, &m_instructionArmed, &m_functionArmed})
        for (const Armed& a : *list)
            if (a.thread == thread && a.addr == pc && a.logMessage.empty()) return true;
    return false;
}

} // namespace debug
