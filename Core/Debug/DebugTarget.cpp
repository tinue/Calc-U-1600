#include "DebugTarget.hpp"

#include <algorithm>

#include "Disasm/LH5801Disassembler.hpp"
#include "Disasm/Z80Disassembler.hpp"

namespace debug {

namespace {

disasm::Decoded decodeAs(CpuKind kind, uint16_t addr, const disasm::FetchFn& fetch, const disasm::SymbolFn& symbols) {
    return kind == CpuKind::Z80 ? disasm::decodeZ80(addr, fetch, symbols) : disasm::decodeLH5801(addr, fetch, symbols);
}

} // namespace

disasm::Decoded DebugTarget::decode(int thread, uint16_t addr, const disasm::SymbolFn& symbols) const {
    auto fetch = [this, thread](uint16_t a) -> uint8_t {
        uint8_t v = 0xFF;
        peek(thread, kSpaceMain, a, &v); // code is fetched from ME0 / Z-80 memory
        return v;
    };
    return decodeAs(kindOf(thread), addr, fetch, symbols);
}

disasm::Decoded DebugTarget::decode(int thread, const HistoryEntry& entry, const disasm::SymbolFn& symbols) const {
    auto fetch = [&entry](uint16_t a) -> uint8_t {
        const uint16_t i = uint16_t(a - entry.pc);
        return i < entry.len ? entry.bytes[i] : 0;
    };
    return decodeAs(kindOf(thread), entry.pc, fetch, symbols);
}

ExpressionContext DebugTarget::expressionContext(int thread,
                                                std::function<bool(const std::string&, int64_t*)> symbols) const {
    ExpressionContext ctx;
    ctx.lookup = [this, thread, symbols](const std::string& name, int64_t* value) {
        uint32_t reg = 0;
        if (readRegister(thread, name, &reg)) { *value = reg; return true; }
        return symbols ? symbols(name, value) : false;
    };
    ctx.readByte = [this, thread](uint16_t addr, bool me1, uint8_t* value) {
        return peek(thread, me1 ? kSpaceME1 : kSpaceMain, addr, value);
    };
    ctx.bigEndian = kindOf(thread) != CpuKind::Z80;
    return ctx;
}

std::string BankKey::describe() const {
    std::string s;
    auto add = [&s](const std::string& part) { s += (s.empty() ? "" : ", ") + part; };
    if (bank >= 0) add("bank " + std::to_string(bank));
    if (me >= 0) add("me" + std::to_string(me));
    if (pu >= 0) add("pu=" + std::to_string(pu));
    if (pv >= 0) add("pv=" + std::to_string(pv));
    return s;
}

bool DebugTarget::bankMatches(int thread, const BankKey& key, uint16_t addr) const {
    if (key.me == 1) return false; // code runs from ME0 only
    if (key.bank >= 0 && bankAt(thread, addr) != key.bank) return false;
    if (key.pu >= 0 && pu(thread) != (key.pu != 0)) return false;
    if (key.pv >= 0 && pv(thread) != (key.pv != 0)) return false;
    return true;
}

void DebugTarget::setBreakpoints(int thread, const std::vector<uint16_t>& addrs) {
    if (thread < 1) return;
    if (m_breakpoints.size() < size_t(thread)) m_breakpoints.resize(size_t(thread));
    std::vector<uint16_t>& list = m_breakpoints[size_t(thread - 1)];
    list = addrs;
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
    applyBreakpoints(thread, list);
    enableBreakpointChecks(m_armed && breakpointsActive());
}

void DebugTarget::arm(bool on) {
    m_armed = on;
    enableBreakpointChecks(on && breakpointsActive());
    for (auto& [thread, set] : m_watches) attachWatches(thread, on && !set.empty() ? &set : nullptr);
}

const std::vector<uint16_t>& DebugTarget::breakpoints(int thread) const {
    static const std::vector<uint16_t> kNone;
    if (thread < 1 || size_t(thread) > m_breakpoints.size()) return kNone;
    return m_breakpoints[size_t(thread - 1)];
}

bool DebugTarget::breakpointsActive() const {
    for (const auto& list : m_breakpoints)
        if (!list.empty()) return true;
    return false;
}

void DebugTarget::setWatches(int thread, const std::vector<WatchSet::Watch>& watches) {
    WatchSet& set = m_watches[thread];
    set.clear();
    for (const auto& w : watches) set.add(w);
    attachWatches(thread, m_armed && !set.empty() ? &set : nullptr);
}

int DebugTarget::watchHitThread() const {
    for (const auto& [thread, set] : m_watches)
        if (set.hitPending()) return thread;
    return 0;
}

void DebugTarget::resumeFromStop() {
    // A CPU parked on one of its breakpoints executes that instruction on
    // resume instead of stopping again. Only CPUs actually sitting on one
    // get the skip, so a parked second CPU can't lose a later stop.
    for (const Thread& t : threads()) {
        const auto& list = breakpoints(t.id);
        if (std::binary_search(list.begin(), list.end(), pc(t.id))) resumePastBreakpoint(t.id);
    }
    for (auto& entry : m_watches)
        if (entry.second.hitPending()) entry.second.consumeHit();
}

Stop DebugTarget::watchStop(int thread) {
    Stop s;
    auto it = m_watches.find(thread);
    if (it == m_watches.end() || !it->second.hitPending()) return s;
    s.kind = Stop::Watch;
    s.thread = thread;
    s.hit = it->second.consumeHit();
    return s;
}

const std::vector<std::string>& flagNames(CpuKind kind) {
    static const std::vector<std::string> kLh = {"c", "ie", "z", "v", "h"};
    static const std::vector<std::string> kZ80 = {"c", "n", "pv", "", "h", "", "z", "s"};
    return kind == CpuKind::Z80 ? kZ80 : kLh;
}

} // namespace debug
