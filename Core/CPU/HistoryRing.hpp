#pragma once

#include <cstdint>

// ── Debugger instruction history ─────────────────────────────────────────
//
// A tiny always-on ring of the last few retired instructions per CPU, for
// the debugger's call-stack view (frames 1..20: "after <insn>", each with
// its post-execution registers). Unlike TraceRing it has no mutex and no
// drain cursor: the CPU writes it from step() and the debugger reads it
// from the same (emulation == GUI) thread while the machine is paused, so
// recording one frame costs a handful of plain stores. It is independent of
// the TRACE file ring and of every TRACE_* flag.

/// One retired LH5801/LH5803 instruction (or interrupt entry).
struct LH5801HistoryFrame {
    uint16_t pc{};        ///< P at the start of the instruction (pre-execution)
    uint8_t  bytes[5]{};  ///< the bytes fetched, prefix included (longest form: FD EF pp pp n)
    uint8_t  len{};       ///< number of valid bytes; 0 for an interrupt entry
    uint8_t  cycles{};
    bool     interrupt{}; ///< interrupt acknowledge: `pc` is the interrupted P, no bytes

    // Post-execution registers
    uint8_t  a{};
    uint16_t x{}, y{}, u{}, s{}, p{};
    uint8_t  t{};
    bool     pu{}, pv{};
};

/// One retired SC7852 (Z-80) instruction (or interrupt entry).
struct Z80HistoryFrame {
    uint16_t pc{};        ///< PC at the start of the instruction, a carried prefix included
    uint8_t  bytes[4]{};  ///< the bytes fetched (longest forms: DD CB d op, ED 43 nn nn, DD 21 nn nn)
    uint8_t  len{};       ///< number of valid bytes; 0 for an interrupt entry
    uint8_t  cycles{};
    bool     interrupt{}; ///< interrupt acknowledge: `pc` is the interrupted PC, no bytes

    // Post-execution registers
    uint16_t af{}, bc{}, de{}, hl{};
    uint16_t af2{}, bc2{}, de2{}, hl2{};
    uint16_t ix{}, iy{}, sp{}, pcAfter{};
    uint8_t  i{}, r{}, im{};
    bool     iff1{}, iff2{};
};

template <typename Frame, uint32_t N>
class HistoryRing {
    static_assert(N != 0 && (N & (N - 1)) == 0, "HistoryRing size must be a power of two");

public:
    static constexpr uint32_t kSize = N;

    /// The slot the next push() fills -- written in place by the CPU, then
    /// committed with commit(). Avoids building a frame on the stack and
    /// copying it.
    Frame& next() { return m_ring[m_head & kMask]; }
    void commit() { m_head++; }

    void push(const Frame& f) { next() = f; commit(); }

    /// Number of frames held (at most N).
    uint32_t size() const { return m_head < N ? m_head : N; }

    /// `age` 0 is the most recent frame; valid for age < size().
    const Frame& recent(uint32_t age) const { return m_ring[(m_head - 1 - age) & kMask]; }

    /// Frames recorded since the last clear() -- grows by one per retired
    /// instruction, so a debugger can tell that a CPU has moved on.
    uint32_t total() const { return m_head; }

    void clear() { m_head = 0; }

private:
    static constexpr uint32_t kMask = N - 1;
    Frame m_ring[N]{};
    uint32_t m_head{0};
};
