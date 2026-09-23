#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

// Fixed-size instruction-trace ring shared by the CPU cores. The emulation
// thread push()es one frame per traced instruction; a consumer (GUI, CLI)
// drain()s them in order or peek()s the most recent without consuming.
// N must be a power of two.
template <typename Frame, uint32_t N>
class TraceRing {
    static_assert(N != 0 && (N & (N - 1)) == 0, "TraceRing size must be a power of two");

public:
    static constexpr uint32_t kSize = N;

    void push(const Frame& f) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ring[m_totalWritten & kMask] = f;
        m_totalWritten++;
    }

    /// Copies up to `max` undrained frames, oldest first, and consumes
    /// them. `*outLost` (if given) receives how many were overwritten
    /// before they could be drained.
    uint32_t drain(Frame* out, uint32_t max, uint32_t* outLost) {
        std::lock_guard<std::mutex> lock(m_mutex);
        uint32_t available = m_totalWritten - m_drainCursor;
        uint32_t lost = 0;
        if (available > N) {
            lost = available - N;
            m_drainCursor = m_totalWritten - N;
            available = N;
        }
        if (outLost) *outLost = lost;
        uint32_t n = std::min(max, available);
        for (uint32_t i = 0; i < n; i++) {
            out[i] = m_ring[(m_drainCursor + i) & kMask];
        }
        m_drainCursor += n;
        return n;
    }

    /// Copies the most recent `max` frames (oldest of those first) without
    /// consuming anything -- the drain cursor is untouched.
    uint32_t peek(Frame* out, uint32_t max) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        uint32_t available = std::min(m_totalWritten, N);
        uint32_t n = std::min(max, available);
        uint32_t start = m_totalWritten - n;
        for (uint32_t i = 0; i < n; i++) {
            out[i] = m_ring[(start + i) & kMask];
        }
        return n;
    }

private:
    static constexpr uint32_t kMask = N - 1;
    Frame m_ring[N]{};
    uint32_t m_drainCursor{0};   // next read index
    uint32_t m_totalWritten{0};  // write count so far; also gives the next write index and overflow accounting
    mutable std::mutex m_mutex;
};

// Sorted PC breakpoint list, safe to edit from another thread while the
// emulation thread checks it.
class BreakpointSet {
public:
    void add(uint16_t addr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!std::binary_search(m_addrs.begin(), m_addrs.end(), addr)) {
            m_addrs.insert(std::upper_bound(m_addrs.begin(), m_addrs.end(), addr), addr);
        }
    }
    void remove(uint16_t addr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = std::lower_bound(m_addrs.begin(), m_addrs.end(), addr);
        if (it != m_addrs.end() && *it == addr) m_addrs.erase(it);
    }
    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_addrs.clear();
    }
    /// True (and latches a hit for consumeHit()) if `pc` is a breakpoint.
    bool check(uint16_t pc) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!std::binary_search(m_addrs.begin(), m_addrs.end(), pc)) return false;
        m_hit.store(true, std::memory_order_relaxed);
        return true;
    }
    /// Returns true once per hit.
    bool consumeHit() { return m_hit.exchange(false, std::memory_order_relaxed); }

private:
    std::vector<uint16_t> m_addrs; // sorted ascending
    std::atomic<bool> m_hit{false};
    mutable std::mutex m_mutex;
};
