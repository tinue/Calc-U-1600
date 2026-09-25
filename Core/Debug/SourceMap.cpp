#include "SourceMap.hpp"

#include <algorithm>

namespace debug {

void SourceMap::build(Binding& b) {
    Index idx;
    for (size_t i = 0; i < b.listing.lines.size(); i++) {
        const ListingLine& l = b.listing.lines[i];
        if (l.addr < b.lo || l.addr > b.hi) continue;
        idx.byAddr.emplace(l.addr, i);
        auto& lines = idx.byFileLine[l.file];
        if (!lines.count(l.line)) lines[l.line] = l.addr;
    }
    for (const auto& [name, value] : b.listing.symbols)
        if (!idx.labels.count(value)) idx.labels[value] = name;
    m_index[b.id] = std::move(idx);
}

int SourceMap::addStatic(int thread, Listing listing, const BankKey& key, const std::string& name) {
    Binding b;
    b.id = m_nextId++;
    b.thread = thread;
    b.key = key;
    b.name = name;
    b.listing = std::move(listing);
    build(b);
    m_bindings.push_back(std::move(b));
    return m_bindings.back().id;
}

int SourceMap::addLoaded(int thread, Listing listing, const BankKey& key, uint16_t lo, uint16_t hi, const std::string& name) {
    for (size_t i = m_bindings.size(); i-- > 0;) {
        const Binding& old = m_bindings[i];
        if (old.loaded && old.thread == thread && old.lo <= hi && lo <= old.hi) {
            m_index.erase(old.id);
            m_bindings.erase(m_bindings.begin() + long(i));
        }
    }
    Binding b;
    b.id = m_nextId++;
    b.thread = thread;
    b.key = key;
    b.loaded = true;
    b.lo = lo;
    b.hi = hi;
    b.name = name;
    b.listing = std::move(listing);
    build(b);
    m_bindings.push_back(std::move(b));
    return m_bindings.back().id;
}

void SourceMap::remove(int id) {
    m_bindings.erase(std::remove_if(m_bindings.begin(), m_bindings.end(), [id](const Binding& b) { return b.id == id; }),
                     m_bindings.end());
    m_index.erase(id);
}

const SourceMap::Binding* SourceMap::binding(int id) const {
    for (const Binding& b : m_bindings)
        if (b.id == id) return &b;
    return nullptr;
}

std::vector<const SourceMap::Binding*> SourceMap::ordered() const {
    std::vector<const Binding*> out;
    for (size_t i = m_bindings.size(); i-- > 0;)
        if (m_bindings[i].loaded) out.push_back(&m_bindings[i]);
    for (const Binding& b : m_bindings)
        if (!b.loaded) out.push_back(&b);
    return out;
}

int SourceMap::verify(int id, const BankMatch& match, const CodePeek& peek) {
    for (Binding& b : m_bindings) {
        if (b.id != id) continue;
        b.checked = 0;
        b.mismatched = 0;
        for (const auto& [addr, i] : indexOf(b).byAddr) {
            if (match && !match(b.thread, b.key, addr)) continue;
            const ListingLine& l = b.listing.lines[i];
            bool same = true;
            for (size_t k = 0; k < l.bytes.size(); k++) {
                uint8_t v = 0;
                if (!peek(b.thread, uint16_t(addr + k), &v) || v != l.bytes[k]) { same = false; break; }
            }
            b.checked++;
            if (!same) b.mismatched++;
        }
        b.stale = b.mismatched > 0;
        return b.mismatched;
    }
    return 0;
}

bool SourceMap::lookup(int thread, uint16_t addr, const BankMatch& match, SourceLocation* out, const CodePeek& peek) const {
    for (const Binding* b : ordered()) {
        if (b->stale || b->thread != thread || addr < b->lo || addr > b->hi) continue;
        if (match && !match(thread, b->key, addr)) continue;
        const Index& idx = indexOf(*b);
        auto it = idx.byAddr.upper_bound(addr);
        if (it == idx.byAddr.begin()) continue;
        --it;
        const ListingLine& l = b->listing.lines[it->second];
        if (addr >= l.addr + l.bytes.size()) continue; // between lines: not code from this listing
        if (peek) {
            bool same = true;
            for (size_t k = 0; k < l.bytes.size() && same; k++) {
                uint8_t v = 0;
                same = peek(thread, uint16_t(l.addr + k), &v) && v == l.bytes[k];
            }
            if (!same) continue;
        }
        out->file = b->listing.files[size_t(l.file)];
        out->line = l.line;
        out->binding = b->id;
        return true;
    }
    return false;
}

std::vector<SourceMap::CodeAddress> SourceMap::addressesFor(const std::string& file, int line, int* resolvedLine) const {
    std::vector<CodeAddress> out;
    int best = 0;
    // First the nearest code line at or below `line` over all bindings of
    // the file, then every address that line has.
    for (const Binding* b : ordered()) {
        if (b->stale) continue;
        const auto& files = b->listing.files;
        for (size_t f = 0; f < files.size(); f++) {
            if (files[f] != file) continue;
            const Index& idx = indexOf(*b);
            auto lines = idx.byFileLine.find(int(f));
            if (lines == idx.byFileLine.end()) continue;
            auto it = lines->second.lower_bound(line);
            if (it != lines->second.end() && (best == 0 || it->first < best)) best = it->first;
        }
    }
    if (resolvedLine) *resolvedLine = best;
    if (best == 0) return out;
    for (const Binding* b : ordered()) {
        if (b->stale) continue;
        const auto& files = b->listing.files;
        for (size_t f = 0; f < files.size(); f++) {
            if (files[f] != file) continue;
            const Index& idx = indexOf(*b);
            auto lines = idx.byFileLine.find(int(f));
            if (lines == idx.byFileLine.end()) continue;
            auto it = lines->second.find(best);
            if (it != lines->second.end()) out.push_back({b->thread, it->second, b->key, b->id});
        }
    }
    return out;
}

bool SourceMap::knowsFile(const std::string& file) const {
    for (const Binding* b : ordered()) {
        if (b->stale) continue;
        const Index& idx = indexOf(*b);
        for (size_t f = 0; f < b->listing.files.size(); f++)
            if (b->listing.files[f] == file && idx.byFileLine.count(int(f))) return true;
    }
    return false;
}

bool SourceMap::symbolValue(const std::string& name, uint16_t* value) const {
    for (const Binding* b : ordered()) {
        auto it = b->listing.symbols.find(name);
        if (it != b->listing.symbols.end()) { *value = it->second; return true; }
    }
    return false;
}

std::string SourceMap::symbolAt(int thread, uint16_t addr) const {
    for (const Binding* b : ordered()) {
        if (b->thread != thread) continue;
        const Index& idx = indexOf(*b);
        auto it = idx.labels.find(addr);
        if (it != idx.labels.end()) return it->second;
    }
    return {};
}

} // namespace debug
