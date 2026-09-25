#include "SourceMap.hpp"

#include <algorithm>

namespace debug {

int SourceMap::add(Binding b) {
    b.id = m_nextId++;
    Index& idx = b.index;
    for (size_t i = 0; i < b.listing.lines.size(); i++) {
        const ListingLine& l = b.listing.lines[i];
        if (l.addr < b.lo || l.addr > b.hi) continue;
        idx.byAddr.emplace(l.addr, i);
        auto& lines = idx.byFileLine[l.file];
        if (!lines.count(l.line)) lines[l.line] = l.addr;
    }
    for (const auto& [name, value] : b.listing.symbols)
        if (!idx.labels.count(value)) idx.labels[value] = name;
    m_bindings.push_back(std::move(b));
    return m_bindings.back().id;
}

int SourceMap::addStatic(int thread, Listing listing, const BankKey& key, const std::string& name) {
    Binding b;
    b.thread = thread;
    b.key = key;
    b.name = name;
    b.listing = std::move(listing);
    return add(std::move(b));
}

int SourceMap::addLoaded(int thread, Listing listing, const BankKey& key, uint16_t lo, uint16_t hi, const std::string& name) {
    m_bindings.erase(std::remove_if(m_bindings.begin(), m_bindings.end(),
                                    [&](const Binding& old) {
                                        return old.loaded && old.thread == thread && old.lo <= hi && lo <= old.hi;
                                    }),
                     m_bindings.end());
    Binding b;
    b.thread = thread;
    b.key = key;
    b.loaded = true;
    b.lo = lo;
    b.hi = hi;
    b.name = name;
    b.listing = std::move(listing);
    return add(std::move(b));
}

void SourceMap::remove(int id) {
    m_bindings.erase(std::remove_if(m_bindings.begin(), m_bindings.end(), [id](const Binding& b) { return b.id == id; }),
                     m_bindings.end());
}

const SourceMap::Binding* SourceMap::binding(int id) const {
    for (const Binding& b : m_bindings)
        if (b.id == id) return &b;
    return nullptr;
}

int SourceMap::verify(int id, const BankMatch& match, const CodePeek& peek) {
    for (Binding& b : m_bindings) {
        if (b.id != id) continue;
        b.checked = 0;
        b.mismatched = 0;
        for (const auto& [addr, i] : b.index.byAddr) {
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
    return firstOf([&](const Binding& b) {
        if (b.stale || b.thread != thread || addr < b.lo || addr > b.hi) return false;
        if (match && !match(thread, b.key, addr)) return false;
        auto it = b.index.byAddr.upper_bound(addr);
        if (it == b.index.byAddr.begin()) return false;
        --it;
        const ListingLine& l = b.listing.lines[it->second];
        if (addr >= l.addr + l.bytes.size()) return false; // between lines: not code from this listing
        if (peek) {
            for (size_t k = 0; k < l.bytes.size(); k++) {
                uint8_t v = 0;
                if (!peek(thread, uint16_t(l.addr + k), &v) || v != l.bytes[k]) return false;
            }
        }
        out->file = b.listing.files[size_t(l.file)];
        out->line = l.line;
        out->binding = b.id;
        return true;
    });
}

std::vector<SourceMap::CodeAddress> SourceMap::addressesFor(const std::string& file, int line, int* resolvedLine) const {
    // `f` for each of the file's line tables, over all live bindings.
    auto forEachFileLines = [this, &file](auto f) {
        firstOf([&](const Binding& b) {
            if (b.stale) return false;
            for (size_t i = 0; i < b.listing.files.size(); i++) {
                if (b.listing.files[i] != file) continue;
                auto lines = b.index.byFileLine.find(int(i));
                if (lines != b.index.byFileLine.end()) f(b, lines->second);
            }
            return false;
        });
    };
    // First the nearest code line at or below `line` over all bindings of
    // the file, then every address that line has.
    int best = 0;
    forEachFileLines([&](const Binding&, const std::map<int, uint16_t>& lines) {
        auto it = lines.lower_bound(line);
        if (it != lines.end() && (best == 0 || it->first < best)) best = it->first;
    });
    if (resolvedLine) *resolvedLine = best;
    std::vector<CodeAddress> out;
    if (best == 0) return out;
    forEachFileLines([&](const Binding& b, const std::map<int, uint16_t>& lines) {
        auto it = lines.find(best);
        if (it != lines.end()) out.push_back({b.thread, it->second, b.key, b.id});
    });
    return out;
}

bool SourceMap::knowsFile(const std::string& file) const {
    return firstOf([&file](const Binding& b) {
        if (b.stale) return false;
        for (size_t f = 0; f < b.listing.files.size(); f++)
            if (b.listing.files[f] == file && b.index.byFileLine.count(int(f))) return true;
        return false;
    });
}

bool SourceMap::symbolValue(const std::string& name, uint16_t* value) const {
    return firstOf([&](const Binding& b) {
        auto it = b.listing.symbols.find(name);
        if (it == b.listing.symbols.end()) return false;
        *value = it->second;
        return true;
    });
}

std::string SourceMap::symbolAt(int thread, uint16_t addr) const {
    std::string label;
    firstOf([&](const Binding& b) {
        if (b.thread != thread) return false;
        auto it = b.index.labels.find(addr);
        if (it == b.index.labels.end()) return false;
        label = it->second;
        return true;
    });
    return label;
}

} // namespace debug
