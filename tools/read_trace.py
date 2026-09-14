#!/usr/bin/env python3
"""
read_trace.py — PC-1500/1500A/1600 binary trace reader.

Parses TRACE.bin (renamed from CALCU1500_TRACE.bin in format v2, once the
same file started carrying PC-1600 frames alongside PC-1500's own; written
by the app's "TRACE" debug-panel button or by a preset's `- trace:` step,
both via the Core writer `Core/PC1500/PC1500TraceFile.cpp`) and either:
  • Prints a human-readable text trace to stdout (default)
  • Emits a JSON array (--json)

Structurally mirrors Calc-U-59's own tools/read_trace.py (see plan.md §0),
scaled down to this project's much smaller per-CPU frame structs — the
LH5801/SC7852's registers are plain single-byte/short values, not
TI-59-style packed BCD digit arrays, so there is no nibble-unpacking step
here. Two frame shapes share one file: `PCCpuFrame` (LH5801-shaped -- the
PC-1500/1500A's own CPU, or the PC-1600's LH5803 co-processor) and
`PCZ80CpuFrame` (the PC-1600's SC7852) -- both carry a `cpuId` byte
(0=unspecified/PC-1500, 1=SC7852, 2=LH5803, see Core/TraceTypes.hpp's
`CPU_ID_*`) so a 'trace'/'trace_z80' record always says which physical CPU
emitted it.

Usage:
    python3 read_trace.py TRACE.bin
    python3 read_trace.py --dedup TRACE.bin   # drop consecutive same-PC frames (per CPU)
    python3 read_trace.py --json TRACE.bin
"""

import sys
import struct
import json

# ── Constants (must match Core/PC1500/PC1500TraceFile.cpp) ──

MAGIC   = 0x50433135   # 'PC15'
VERSION = 2            # v2: 25-byte TRACE_EVENT (added cpuId), new TRACE_EVENT_Z80 record

REC_SESSION_START = 0x01
REC_TRACE_EVENT   = 0x02
REC_SESSION_END   = 0x03
REC_TRACE_EVENT_Z80 = 0x04
REC_TRACE_GAP     = 0x05

CPU_ID_UNSPECIFIED = 0
CPU_ID_SC7852       = 1
CPU_ID_LH5803        = 2
CPU_ID_NAMES = {CPU_ID_UNSPECIFIED: '-', CPU_ID_SC7852: 'SC7852', CPU_ID_LH5803: 'LH5803'}

# T register bit layout, per Core/TraceTypes.hpp: bit0=C,1=IE,2=Z,3=V,4=H
T_BIT_C  = 0x01
T_BIT_IE = 0x02
T_BIT_Z  = 0x04
T_BIT_V  = 0x08
T_BIT_H  = 0x10

# ── Low-level reader ────────────────────────────────────────────────────────

def _read_exact(f, n):
    data = f.read(n)
    if len(data) != n:
        raise EOFError(f"Expected {n} bytes, got {len(data)}")
    return data

def _parse_file_header(f):
    hdr = _read_exact(f, 16)
    magic, version = struct.unpack_from('<IH', hdr, 0)
    if magic != MAGIC:
        raise ValueError(f"Bad magic: 0x{magic:08X} (expected 0x{MAGIC:08X})")
    if version != VERSION:
        raise ValueError(f"Unsupported version: {version} (expected {VERSION})")
    return version

def _parse_trace_event(payload):
    """25-byte TRACE_EVENT payload (v2, LH5801-shaped) -> dict. See
    Core/PC1500/PC1500TraceFile.cpp's frame-writing code for the exact field
    order this mirrors. Used by both the PC-1500/1500A's own CPU and the
    PC-1600's LH5803 co-processor -- 'cpuId' says which."""
    if len(payload) != 25:
        raise ValueError(f"TRACE_EVENT payload length {len(payload)}, expected 25")

    (seqno, pc, opcode, cycles, a, xl, xh, yl, yh, ul, uh,
     s, t, pu, pv, disp, tm, cpu_id) = struct.unpack_from('<IHHB BBBBBBB HB BBB H B', payload, 0)

    return {
        'type':   'trace',
        'cpuId':  cpu_id,
        'cpu':    CPU_ID_NAMES.get(cpu_id, f'?{cpu_id}'),
        'seqno':  seqno,
        'pc':     f'{pc:04X}',
        'opcode': f'{opcode:04X}',
        'cycles': cycles,
        'A':      f'{a:02X}',
        'XL':     f'{xl:02X}', 'XH': f'{xh:02X}',
        'YL':     f'{yl:02X}', 'YH': f'{yh:02X}',
        'UL':     f'{ul:02X}', 'UH': f'{uh:02X}',
        'S':      f'{s:04X}',
        'T':      t,
        'C':      1 if (t & T_BIT_C) else 0,
        'IE':     1 if (t & T_BIT_IE) else 0,
        'Z':      1 if (t & T_BIT_Z) else 0,
        'V':      1 if (t & T_BIT_V) else 0,
        'H':      1 if (t & T_BIT_H) else 0,
        'PU':     f'{pu:02X}', 'PV': f'{pv:02X}',
        'DISP':   disp,
        'TM':     tm,
    }

def _parse_trace_event_z80(payload):
    """29-byte TRACE_EVENT_Z80 payload -> dict (the PC-1600's SC7852). See
    Core/PC1500/PC1500TraceFile.cpp's Z80 frame-writing code for the exact
    field order this mirrors."""
    if len(payload) != 29:
        raise ValueError(f"TRACE_EVENT_Z80 payload length {len(payload)}, expected 29")

    (seqno, pc, opcode, cycles, cpu_id, af, bc, de, hl, ix, iy, sp,
     i_reg, r_reg, iff1, iff2, im) = struct.unpack_from('<IHHBB HHHHHHH BBBBB', payload, 0)

    return {
        'type':   'trace_z80',
        'cpuId':  cpu_id,
        'cpu':    CPU_ID_NAMES.get(cpu_id, f'?{cpu_id}'),
        'seqno':  seqno,
        'pc':     f'{pc:04X}',
        'opcode': f'{opcode:04X}',
        'cycles': cycles,
        'AF':     f'{af:04X}', 'BC': f'{bc:04X}', 'DE': f'{de:04X}', 'HL': f'{hl:04X}',
        'IX':     f'{ix:04X}', 'IY': f'{iy:04X}', 'SP': f'{sp:04X}',
        'I':      f'{i_reg:02X}', 'R': f'{r_reg:02X}',
        'IFF1':   iff1, 'IFF2': iff2,
        'IM':     im,
    }

def _parse_session_start(payload):
    ts, = struct.unpack_from('<Q', payload, 0)
    return {'type': 'session_start', 'timestamp': ts}

def _parse_session_end(payload):
    count, = struct.unpack_from('<I', payload, 0)
    return {'type': 'session_end', 'eventCount': count}

def _parse_trace_gap(payload):
    count, = struct.unpack_from('<I', payload, 0)
    return {'type': 'gap', 'lostFrames': count}

# ── Public API ────────────────────────────────────────────────────────────

def load_trace(path):
    """Parse CALCU1500_TRACE.bin and return a list of record dicts.

    Record types: 'session_start', 'session_end', 'trace', 'gap'.
    Each 'trace' record is tagged with '_trace_index' (1-indexed).
    """
    records = []
    trace_index = 0
    with open(path, 'rb') as f:
        try:
            _parse_file_header(f)
        except (ValueError, EOFError) as exc:
            raise ValueError(f"Cannot read {path}: {exc}") from exc

        while True:
            hdr = f.read(3)
            if len(hdr) == 0:
                break
            if len(hdr) < 3:
                break  # truncated file -- stop gracefully
            rec_type = hdr[0]
            payload_len = struct.unpack_from('<H', hdr, 1)[0]
            payload = f.read(payload_len)

            if rec_type == REC_SESSION_START:
                records.append(_parse_session_start(payload))
            elif rec_type == REC_TRACE_EVENT:
                trace_index += 1
                rec = _parse_trace_event(payload)
                rec['_trace_index'] = trace_index
                records.append(rec)
            elif rec_type == REC_TRACE_EVENT_Z80:
                trace_index += 1
                rec = _parse_trace_event_z80(payload)
                rec['_trace_index'] = trace_index
                records.append(rec)
            elif rec_type == REC_SESSION_END:
                records.append(_parse_session_end(payload))
            elif rec_type == REC_TRACE_GAP:
                records.append(_parse_trace_gap(payload))
            # Unknown types: skip (forward-compatible per spec)

    return records

def trace_events_only(records):
    """Filter to just 'trace'/'trace_z80' records (drops session/gap markers)."""
    return [r for r in records if r['type'] in ('trace', 'trace_z80')]

# ── Human-readable formatter ────────────────────────────────────────────────

def _format_trace_record(rec):
    flags = ''.join([
        'C' if rec['C'] else '-',
        'I' if rec['IE'] else '-',
        'Z' if rec['Z'] else '-',
        'V' if rec['V'] else '-',
        'H' if rec['H'] else '-',
    ])
    line1 = (f"{rec['_trace_index']:>8d} [{rec['cpu']:>6s}]  PC={rec['pc']} OP={rec['opcode']} "
             f"cyc={rec['cycles']:<3d} T={flags}")
    line2 = (f"          A={rec['A']} X={rec['XH']}{rec['XL']} Y={rec['YH']}{rec['YL']} "
              f"U={rec['UH']}{rec['UL']} S={rec['S']}")
    line3 = f"          PU={rec['PU']} PV={rec['PV']} DISP={rec['DISP']} TM={rec['TM']}"
    return '\n'.join([line1, line2, line3])

def _format_trace_record_z80(rec):
    line1 = (f"{rec['_trace_index']:>8d} [{rec['cpu']:>6s}]  PC={rec['pc']} OP={rec['opcode']} "
             f"cyc={rec['cycles']:<3d} IFF1={rec['IFF1']} IFF2={rec['IFF2']} IM={rec['IM']}")
    line2 = (f"          AF={rec['AF']} BC={rec['BC']} DE={rec['DE']} HL={rec['HL']} "
             f"IX={rec['IX']} IY={rec['IY']} SP={rec['SP']}")
    line3 = f"          I={rec['I']} R={rec['R']}"
    return '\n'.join([line1, line2, line3])

def _apply_dedup(records):
    """Keep only records where PC changes, tracked separately per CPU;
    non-trace records always kept."""
    deduped = []
    last_pc = {}
    for rec in records:
        if rec['type'] not in ('trace', 'trace_z80'):
            deduped.append(rec)
        else:
            cpu_id = rec['cpuId']
            if rec['pc'] != last_pc.get(cpu_id):
                deduped.append(rec)
                last_pc[cpu_id] = rec['pc']
    return deduped

def format_as_log(records, dedup=False):
    if dedup:
        records = _apply_dedup(records)

    out = []
    for rec in records:
        t = rec['type']
        if t == 'session_start':
            import datetime
            ts = datetime.datetime.fromtimestamp(rec['timestamp'])
            out.append(f"\n; SESSION START  {ts.isoformat()}\n")
        elif t == 'session_end':
            out.append(f"; SESSION END  events={rec['eventCount']}\n")
        elif t == 'gap':
            out.append(f"; GAP  lostFrames={rec['lostFrames']}\n")
        elif t == 'trace':
            out.append(_format_trace_record(rec))
        elif t == 'trace_z80':
            out.append(_format_trace_record_z80(rec))
    return '\n'.join(out)

# ── Entry point ─────────────────────────────────────────────────────────────

def main():
    args = sys.argv[1:]
    as_json = '--json' in args
    dedup = '--dedup' in args
    paths = [a for a in args if not a.startswith('--')]
    if not paths:
        print(__doc__)
        sys.exit(1)

    for path in paths:
        records = load_trace(path)
        if as_json:
            print(json.dumps(records, indent=2))
        else:
            print(format_as_log(records, dedup=dedup))

if __name__ == '__main__':
    main()
