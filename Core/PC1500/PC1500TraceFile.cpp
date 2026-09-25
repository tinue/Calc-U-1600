#include "PC1500TraceFile.hpp"

#include <ctime>

namespace {

// ── Format constants ─────────────────────────────────────────────────────
// MUST match tools/read_trace.py's constant block.
constexpr uint32_t kMagic   = 0x50433135;  // 'PC15'
constexpr uint16_t kVersion = 2;  // v2: 25-byte TRACE_EVENT (added cpuId), new TRACE_EVENT_Z80 record
constexpr int      kHeaderSize = 16;

enum RecType : uint8_t {
    kRecSessionStart  = 0x01,
    kRecTraceEvent    = 0x02, // CpuFrame (LH5801-shaped): PC-1500/1500A's own CPU, or PC-1600's LH5803
    kRecSessionEnd    = 0x03,
    kRecTraceGap      = 0x05,
    kRecTraceEventZ80 = 0x04, // Z80CpuFrame-shaped: PC-1600's SC7852
};

// Little-endian append helpers into a fixed-size local byte buffer.
struct ByteSink {
    uint8_t* p;
    void u8(uint8_t v) { *p++ = v; }
    void u16(uint16_t v) {
        *p++ = uint8_t(v & 0xFF);
        *p++ = uint8_t(v >> 8);
    }
    void u32(uint32_t v) {
        *p++ = uint8_t(v & 0xFF);
        *p++ = uint8_t((v >> 8) & 0xFF);
        *p++ = uint8_t((v >> 16) & 0xFF);
        *p++ = uint8_t((v >> 24) & 0xFF);
    }
    void u64(uint64_t v) {
        u32(uint32_t(v & 0xFFFFFFFFu));
        u32(uint32_t(v >> 32));
    }
};

} // namespace

PC1500TraceFile::PC1500TraceFile(std::FILE* handle) : m_fh(handle) {
    if (!m_fh) return;

    uint8_t header[kHeaderSize];
    ByteSink h{header};
    h.u32(kMagic);
    h.u16(kVersion);
    h.u16(0);   // reserved (was "model" in Calc-U-59's v2 -- single model here)
    h.u64(0);   // reserved for future use
    m_bytesWritten += std::fwrite(header, 1, sizeof(header), m_fh);

    uint8_t payload[8];
    ByteSink s{payload};
    s.u64(static_cast<uint64_t>(std::time(nullptr)));
    writeRecord(kRecSessionStart, payload, sizeof(payload));
}

PC1500TraceFile::~PC1500TraceFile() { finish(); }

void PC1500TraceFile::writeRecord(uint8_t type, const uint8_t* payload, uint16_t len) {
    if (!m_fh) return;
    uint8_t recHeader[3];
    ByteSink h{recHeader};
    h.u8(type);
    h.u16(len);
    m_bytesWritten += std::fwrite(recHeader, 1, sizeof(recHeader), m_fh);
    if (len) m_bytesWritten += std::fwrite(payload, 1, len, m_fh);
}

void PC1500TraceFile::writeFrame(const CpuFrame& f) {
    if (!m_fh) return;
    // 25-byte TRACE_EVENT payload -- field order MUST match
    // TraceWriter.swift's makeFramePayload() and read_trace.py's
    // `<IHHB BBBBBBB HB BBB H B` unpack (trailing B = cpuId, v2).
    uint8_t payload[25];
    ByteSink s{payload};
    s.u32(f.seqno);
    s.u16(f.pc);
    s.u16(f.opcode);
    s.u8(f.cycles);
    s.u8(f.a);
    s.u8(f.xl);
    s.u8(f.xh);
    s.u8(f.yl);
    s.u8(f.yh);
    s.u8(f.ul);
    s.u8(f.uh);
    s.u16(f.s);
    s.u8(f.t);
    s.u8(f.pu);
    s.u8(f.pv);
    s.u8(f.disp);
    s.u16(f.tm);
    s.u8(f.cpuId);
    writeRecord(kRecTraceEvent, payload, sizeof(payload));
    m_eventCount++;
}

void PC1500TraceFile::writeFrame(const Z80CpuFrame& f) {
    if (!m_fh) return;
    // 29-byte TRACE_EVENT_Z80 payload -- field order MUST match
    // TraceWriter.swift's makeZ80FramePayload() and read_trace.py's
    // `<IHHBB HHHHHHH BBBBB` unpack.
    uint8_t payload[29];
    ByteSink s{payload};
    s.u32(f.seqno);
    s.u16(f.pc);
    s.u16(f.opcode);
    s.u8(f.cycles);
    s.u8(f.cpuId);
    s.u16(f.af);
    s.u16(f.bc);
    s.u16(f.de);
    s.u16(f.hl);
    s.u16(f.ix);
    s.u16(f.iy);
    s.u16(f.sp);
    s.u8(f.i);
    s.u8(f.r);
    s.u8(f.iff1 ? 1 : 0);
    s.u8(f.iff2 ? 1 : 0);
    s.u8(f.im);
    writeRecord(kRecTraceEventZ80, payload, sizeof(payload));
    m_eventCount++;
}

void PC1500TraceFile::writeGap(uint32_t lostFrames) {
    if (!m_fh) return;
    uint8_t payload[4];
    ByteSink s{payload};
    s.u32(lostFrames);
    writeRecord(kRecTraceGap, payload, sizeof(payload));
}

void PC1500TraceFile::finish() {
    if (!m_fh) return;
    uint8_t payload[4];
    ByteSink s{payload};
    s.u32(m_eventCount);
    writeRecord(kRecSessionEnd, payload, sizeof(payload));
    std::fflush(m_fh);
    std::fclose(m_fh);
    m_fh = nullptr;
}
