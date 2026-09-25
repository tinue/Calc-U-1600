#pragma once
#include <cstdint>
#include <cstdio>

#include "../TraceTypes.hpp"

// ── PC1500TraceFile ────────────────────────────────────────────────────────
//
// Core-side writer for the `TRACE.bin` binary instruction-trace format
// (renamed from `CALCU1500_TRACE.bin` in format v2, once the same file
// started carrying PC-1600 frames too). This is the *only* writer of the
// format -- both the headless preset loader's `trace:` step and the GUI's
// live "TRACE" debug-panel button (Qt6/app/DebugPanel.cpp) go through this
// same class, so there is nothing else that needs to stay in sync with it
// except the reader: `tools/read_trace.py`'s constant block and frame
// parsing MUST move together with this file's magic, version, the
// record-type enum, and both TRACE_EVENT field orders (25-byte
// LH5801-shaped -- CpuFrame, used by both the PC-1500/1500A's own CPU and
// the PC-1600's LH5803 co-processor -- and 29-byte Z80-shaped --
// Z80CpuFrame, the PC-1600's SC7852).
//
// Both CPUs' frames land in ONE file (not two), tagged by `cpuId`
// (CpuFrame/Z80CpuFrame's own field, Core/TraceTypes.hpp) -- safe because
// PC1600BusArbiter guarantees only one CPU ever executes at a time (no
// real concurrency to race), and each CPU's own trace ring has an
// independent internal mutex, so draining both every tick can't deadlock
// against anything.
//
// It is handed an already-open `FILE*` -- opened by the preset loader (CLI)
// or by the GUI's debug panel -- and takes ownership of it: `finish()`
// (also called by the destructor) is what `fclose`s it.
//
// Not thread-safe. `PC1500Machine` only ever touches its instance from
// whichever thread is stepping the CPU, under the machine mutex.
class PC1500TraceFile {
public:
    /// Takes ownership of `handle` (must be open for binary writing).
    /// Immediately writes the 16-byte file header and a SESSION_START
    /// record. If `handle` is null the object is inert (every method is a
    /// no-op) -- callers that care should check the handle before
    /// constructing.
    explicit PC1500TraceFile(std::FILE* handle);

    PC1500TraceFile(const PC1500TraceFile&) = delete;
    PC1500TraceFile& operator=(const PC1500TraceFile&) = delete;

    /// Writes one TRACE_EVENT record (LH5801-shaped -- the PC-1500/1500A's
    /// own CPU, or the PC-1600's LH5803 co-processor). Field groups not
    /// populated by the active TRACE_* flags are simply written as their
    /// zero-initialised `CpuFrame` values, matching TraceWriter.swift.
    void writeFrame(const CpuFrame& f);
    /// Writes one TRACE_EVENT_Z80 record (the PC-1600's SC7852).
    void writeFrame(const Z80CpuFrame& f);

    /// Writes a TRACE_GAP record: `lostFrames` instructions were
    /// overwritten in the ring before they could be drained.
    void writeGap(uint32_t lostFrames);

    /// Writes a SESSION_END record (with the running event count), flushes,
    /// and `fclose`s the handle. Idempotent -- a second call is a no-op.
    void finish();

    /// Bytes handed to the file so far (header included) -- the file's
    /// size once stdio flushes, without asking the filesystem.
    uint64_t bytesWritten() const { return m_bytesWritten; }

    ~PC1500TraceFile();

private:
    std::FILE* m_fh = nullptr;
    uint32_t m_eventCount = 0;
    uint64_t m_bytesWritten = 0;

    void writeRecord(uint8_t type, const uint8_t* payload, uint16_t len);
};
