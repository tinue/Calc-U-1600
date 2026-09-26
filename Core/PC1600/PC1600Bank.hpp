#pragma once
#include <cstdint>

// ── PC-1600 bank-select registers ────────────────────────────────────────
//
// Pure register/decode logic — no CPU or memory-storage dependency, so
// it's directly truth-table-testable with synthetic port writes.
// Implements the model from the Technical Reference Manual's §7.2.1
// bank-select table.
//
// Three registers, each covering a disjoint slice of the SC7852's Z-80
// address space:
//
//   Port 31H (`IOW MAP`/`IOR MAP`, R/W) — four INDEPENDENT bit fields, none
//   sharing a bit:
//     b0        (PVOUT)  1-bit bank select, page A only (0000-3FFF)
//     b3:b2:b1  (3-bit)  bank select 0-7,   page B only (4000-7FFF)
//     b6:b5:b4  (3-bit)  bank select 0-7,   page C only (8000-BFFF)
//                        — b6 is ALSO, independently, the LHS1/LHS2/LHS3
//                          remap-table selector (Bank 0 windows, from
//                          PC-1600-Memory-Bank-Switching.md Part 4; not
//                          modelled, nothing in the ROM paths we run needs it):
//                            b6=0: LHS1 A800, LHS2 B000, LHS3 B800
//                            b6=1: LHS1 B000, LHS2 A800, LHS3 A000
//     b7        1-bit    bank select, page D only (C000-FFFF)
//
//   Port 28H (write-only) — Slot 2 "vertical bank" select, 0-7 (superRAM
//   hardware extends this to 0-15, exposed for parity even though nothing
//   in this project reads past 7). Orthogonal to Port 31H's page-C field —
//   this selects which 32KB "vertical bank" the module attached to Slot 2
//   exposes, decoded on the module side, not the mainboard. This class
//   only remembers the last-written value (slot2VerticalBank(), consumed
//   by the debug panel and the bank tests); the live vertical-bank decode
//   lives in the card itself, whose latch samples the Port 28H data-bus
//   write forwarded by PC1600Memory::writeIO (see MemorySlotConnector::
//   ioWrite and the CE-1601M card definition). vertical bank 0 is the only
//   one usable for program/expansion memory; banks 1+ are RAM/ROM-file
//   storage (per the CE-1601M manual).
//
//   Port 3DH (write-only) — bit 2 clear selects the hidden Bank 3b BASIC
//   ROM at page B (4000-7FFF) in place of the normal Bank 3 ROM. Not
//   readable via IN — mirrored at system memory address F07DH instead (see
//   PC1600Memory). D0-D2 additionally latch into gate-array outputs
//   A14A-A16A; A16A extends CS24's 16KB window into 32KB by sub-banking
//   Bank 3 into two 8KB halves, which is a memory-decode detail
//   (PC1600Memory), not something this register class needs to interpret.
//
//   Port 3CH (write-only) — the SLOT1MAP (0196H) / SLOT2MAP (0199H)
//   gate-array control. The firmware keeps a RAM shadow at F08DH and
//   re-derives the port byte from it; the SLOT2MAP ROM routine
//   (PC1600-P0-B0-new.bin 0A6DH) shows the layout:
//     b2      SLOT1MAP: 0 = Slot 1 at page-C banks 0/1 (default); 1 =
//             Slot 1's high 16KB half (beta) ALSO answers at page-B bank 1
//             (4000-7FFF), in addition to its normal page-C bank-1 home --
//             a mirror, not a move. TRM 0196H entry + diagram (English +
//             German editions agree): alpha (Slot 1's low half) stays at
//             page-C bank 0 either way; beta appears at page-C bank 1 AND
//             page-B bank 1 when SLOT1MAP=1.
//     b5:b4   SLOT2MAP: 00 = Slot 2 at page-C banks 2/3 (default);
//             10 = Slot 2's first 16KB also answers at page-C bank 1;
//             01 = Slot 2's first 16KB at page-B bank 1 (4000-7FFF) and
//                  its last 16KB at page-A bank 1 (0000-3FFF).
//   PC1600Memory turns b2/b5:b4 into effective-address rewrites feeding the
//   ordinary Slot 1/Slot 2 decode (its class comment's "never as a
//   connector pin" note; kSlot1Map/kSlot2Map). b5:b4 = 11 never occurs.
//
//   SLOT1MAP=1 and SLOT2MAP mode 2 both mirror onto the same window (page-B
//   bank 1) — a real bus collision between two different cards if firmware
//   ever arms both at once (undocumented on real hardware). Resolved by
//   "last call wins": whichever of the two fields most recently *changed
//   value* takes the window — see slot1MapWinsTie() / m_slot1MapChangeSeq.
class PC1600Bank {
public:
    // ── Port 31H ──────────────────────────────────────────────────────
    void    writePort31(uint8_t value) { m_port31 = value; }
    uint8_t readPort31() const { return m_port31; }

    /// Page A (0000-3FFF) bank select: 0 or 1 (Port 31H bit 0, PVOUT).
    uint8_t pageABank() const { return m_port31 & 0x01; }
    /// Page B (4000-7FFF) bank select: 0-7 (Port 31H bits 1-3).
    uint8_t pageBBank() const { return (m_port31 >> 1) & 0x07; }
    /// Page C (8000-BFFF) bank select: 0-7 (Port 31H bits 4-6).
    uint8_t pageCBank() const { return (m_port31 >> 4) & 0x07; }
    /// Page D (C000-FFFF) bank select: 0 or 1 (Port 31H bit 7).
    uint8_t pageDBank() const { return (m_port31 >> 7) & 0x01; }

    // ── Port 28H ──────────────────────────────────────────────────────
    void    writePort28(uint8_t value) { m_port28 = value; }
    /// Last value written to Port 28H (0-7 on stock hardware, 0-15 on
    /// superRAM). Not independently readable on real hardware; exposed
    /// here for debug/test purposes only.
    uint8_t slot2VerticalBank() const { return m_port28; }

    // ── Port 3CH (SLOT1MAP / SLOT2MAP gate-array control) ─────────────
    /// Tracks, on top of the raw byte, which of SLOT1MAP's bit / SLOT2MAP's
    /// 2-bit field last *changed value* — see the class comment's "last
    /// call wins" note and slot1MapWinsTie().
    void writePort3C(uint8_t value) {
        ++m_port3cSeq;
        if ((m_port3c & 0x04) != (value & 0x04)) m_slot1MapChangeSeq = m_port3cSeq;
        if ((m_port3c & 0x30) != (value & 0x30)) m_slot2MapChangeSeq = m_port3cSeq;
        m_port3c = value;
    }
    uint8_t readPort3C() const { return m_port3c; }

    /// SLOT1MAP active from Port 3CH b2 — false (default: Slot 1 at page-C
    /// banks 0/1 only) or true (Slot 1's high half also answers at page-B
    /// bank 1). See PC1600Memory::slot1MapTarget().
    bool slot1MapActive() const { return (m_port3c & 0x04) != 0; }

    /// SLOT2MAP mode from Port 3CH b5:b4 — 0 (default: Slot 2 at page-C
    /// banks 2/3), 1 (also at page-C bank 1), or 2 (at page-B bank 1 /
    /// page-A bank 1). See PC1600Memory::slot2MapTarget().
    uint8_t slot2MapMode() const {
        if (m_port3c & 0x20) return 1;
        if (m_port3c & 0x10) return 2;
        return 0;
    }

    /// Arbitrates the SLOT1MAP=1 / SLOT2MAP-mode-2 collision at page-B bank
    /// 1 (the only window both can claim at once): true if Slot 1 wins,
    /// i.e. SLOT1MAP's bit changed more recently than SLOT2MAP's field did
    /// ("last call wins" — see the class comment). On an exact tie (both
    /// changed in the very same Port 3CH write) Slot 2 wins, matching
    /// SLOT2MAP's existing precedence when only its own modes overlap.
    /// Meaningless (and not called) unless both are simultaneously active.
    bool slot1MapWinsTie() const { return m_slot1MapChangeSeq > m_slot2MapChangeSeq; }

    /// Applies the above arbitration to a pair of "does this redirect claim
    /// the access" flags: if both are set, clears whichever one loses the
    /// tie; no-op otherwise. Shared by PC1600Memory::resolveSlotRemap()
    /// (live read/write decode) and PC1600Machine::debugBankState() (debug
    /// decode) so the "last call wins" rule lives in exactly one place
    /// instead of being hand-duplicated at each site.
    void resolveSlotCollision(bool* slot1Hit, bool* slot2Hit) const {
        if (*slot1Hit && *slot2Hit) {
            if (slot1MapWinsTie()) *slot2Hit = false;
            else                   *slot1Hit = false;
        }
    }

    // ── Port 3DH ──────────────────────────────────────────────────────
    void    writePort3D(uint8_t value) { m_port3d = value; }
    /// Last value written to Port 3DH — PC1600Memory derives the hidden-ROM
    /// latch and F07DH mirror from this directly.
    uint8_t port3DLatch() const { return m_port3d; }
    /// True when the hidden Bank 3b BASIC ROM is selected in place of the
    /// normal Bank 3 ROM at page B (Port 3DH bit 2 clear).
    bool hiddenBasicRomSelected() const { return (m_port3d & 0x04) == 0; }

    /// Resets all four registers to 0 — the documented reset-state bank
    /// configuration (A13A high/A15A low/A14A high per
    /// PC-1600-Machine-Overview.md §6) corresponds to an all-zero register
    /// bank: page A/B/C/D all select bank 0, hidden ROM not selected.
    void reset() {
        m_port31 = 0;
        m_port28 = 0;
        m_port3c = 0;
        m_port3d = 0;
        m_port3cSeq = 0;
        m_slot1MapChangeSeq = 0;
        m_slot2MapChangeSeq = 0;
    }

private:
    uint8_t m_port31{0};
    uint8_t m_port28{0};
    uint8_t m_port3c{0};
    uint8_t m_port3d{0};
    // "Last call wins" bookkeeping for the SLOT1MAP/SLOT2MAP collision at
    // page-B bank 1 — see writePort3C()/slot1MapWinsTie().
    uint32_t m_port3cSeq{0};
    uint32_t m_slot1MapChangeSeq{0};
    uint32_t m_slot2MapChangeSeq{0};
};
