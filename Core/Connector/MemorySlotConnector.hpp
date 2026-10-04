#pragma once
#include <cstdint>

#include "CardChain.hpp"
#include "ExpansionCard.hpp"
#include "../PC1600/PC1600Bank.hpp"

// ── PC-1600 40-pin memory-slot connector ────────────────────────────────
//
// The PC-1600 has two of these, one per memory bay (TRM §10.3(2)/(3),
// Ref/Shared/Expansion-Connectors.md §4). Sibling of the PC-1500's ExpansionConnector:
// exactly one card slot, consulted by PC1600Memory on every page-C
// (8000-BFFF) slot-bank access -- never a memory-map shortcut.
//
// This class is what knows the PC-1600. It drives each physical pin with
// the signal that bay's TRM pin table says the contact carries, and the
// attached card decodes purely from those pins (it never learns which host
// or slot it is in -- see ExpansionCard.hpp). The two bays differ only on
// pins 4/16/17/18; every other contact is identical.
//
// Per Ref/Shared/Expansion-Connectors.md §4.2a this project trusts the module-hardware
// evidence over the TRM's own (swapped) section headers: the physical
// Slot 1 bay carries RAM2 + S1/S2/S3, the physical Slot 2 bay carries
// RAM1 + K0/K1/K2.
class MemorySlotConnector {
public:
    enum class Slot { Slot1, Slot2 };

    MemorySlotConnector(Slot slot, const PC1600Bank& bank) : m_slot(slot), m_bank(bank) {}

    void attach(ExpansionCard* card) { m_card.replace(card); }
    void detach() { m_card.clear(); }
    ExpansionCard* attachedCard() const { return m_card.front(); }

    /// The attached card's own current bank (ExpansionCard::debugCurrentBank()),
    /// or -1 when the slot is empty or the card has no bank concept. For
    /// the GUI debug "Dump Mem" panel only.
    int attachedCardBank() const { return m_card.empty() ? -1 : m_card.front()->debugCurrentBank(); }
    /// The attached card's own total bank count (ExpansionCard::debugBankCount()),
    /// or -1 when the slot is empty or the card has no bank concept. GUI
    /// debug resource-inventory ("N-way paging" label) only.
    int attachedCardBankCount() const { return m_card.empty() ? -1 : m_card.front()->debugBankCount(); }

    /// True when a card is plugged in AND this slot is the bank currently
    /// routed to 8000-BFFF, i.e. its RAM2#/RAM1# chip select is asserted.
    /// Checked before `decode()` builds a PinState so an unselected slot
    /// costs a couple of compares rather than a full pin snapshot -- this
    /// sits on PC1600Memory's per-access path.
    bool selected(uint16_t addr) const { return !m_card.empty() && chipSelect(addr); }

    bool read(uint16_t addr, uint8_t& out) const {
        if (!selected(addr)) return false;
        return m_card.read(decode(addr, /*forWrite=*/false), out);
    }

    /// `direct` = true for a host poke() (debug / preset loader) rather than
    /// a guest-CPU store -- forwarded via PinState::direct so a lock-gating
    /// card can bypass its runtime write protocol.
    WriteResult write(uint16_t addr, uint8_t value, bool direct = false) {
        if (!selected(addr)) return WriteResult::ignored();
        PinState pins = decode(addr, /*forWrite=*/true);
        pins.direct = direct;
        return m_card.write(pins, value);
    }

    /// A host poke / peek at `addr` ($8000-$BFFF) in global page-C `bank`
    /// (0-3), whichever bank is mapped right now: the card sees the pins
    /// decode() drives when the CPU accesses that bank, so its own wiring
    /// places the byte (a CE-155's S1 chip at A000H is not image offset
    /// 2000H). For the fast loaders. A bank of the other slot, or an empty
    /// slot: ignored / false.
    WriteResult writeInBank(int bank, uint16_t addr, uint8_t value) {
        if (!servesBank(bank, addr)) return WriteResult::ignored();
        PinState pins = decode(addr, /*forWrite=*/true, bank);
        pins.direct = true;
        return m_card.write(pins, value);
    }
    bool readInBank(int bank, uint16_t addr, uint8_t& out) const {
        if (!servesBank(bank, addr)) return false;
        return m_card.read(decode(addr, /*forWrite=*/false, bank), out);
    }

    /// A SLOT2MAP (or SLOT1MAP) gate-array remap access -- see
    /// PC1600Memory::slot2MapTarget(). The mainboard has asserted this
    /// slot's RAM chip-select for a window *other* than its normal page-C
    /// bank, so `selected()` / `decode()`'s Port-31H-derived bank & PVOUT
    /// view doesn't apply: the caller has already resolved the half-select
    /// (`pvoutHigh` -- Slot 2's PVOUT / SRAM A14) and the 0..0x3FFF offset
    /// within that 16 KB half. A CE-1601M-class card's own Port 28H
    /// vertical-bank latch still applies -- that's internal card state, not
    /// derived here.
    bool readRemapped(uint16_t offset, bool pvoutHigh, uint8_t& out) const {
        if (m_card.empty()) return false;
        return m_card.read(remapPins(offset, pvoutHigh, /*forWrite=*/false), out);
    }
    WriteResult writeRemapped(uint16_t offset, bool pvoutHigh, uint8_t value, bool direct) {
        if (m_card.empty()) return WriteResult::ignored();
        PinState p = remapPins(offset, pvoutHigh, /*forWrite=*/true);
        p.direct = direct;
        return m_card.write(p, value);
    }

    /// An I/O-space write to a slot I/O port (`OUT (n),A`) -- the PC-1600
    /// routes the Slot-2 range (28-2FH) here. A card whose bank latch
    /// triggers on a port write (CE-1601M: `OUT (28H)`) samples `value`
    /// (the data bus). Not gated by `selected()` -- an I/O write happens
    /// regardless of which page-C bank is mapped.
    bool ioWrite(uint8_t port, uint8_t value) {
        if (m_card.empty()) return false;
        PinState pins;
        pins.forWrite = true;
        pins.ioWrite = true;
        pins.address = port;
        return m_card.write(pins, value);
    }

private:
    Slot m_slot;
    const PC1600Bank& m_bank;
    CardChain<ExpansionCard, PinState> m_card; // a chain of one: the bay takes one module

    // Pin 4 -- RAM2# (Slot 1) / RAM1# (Slot 2), generated by the mainboard.
    // Ref/PC-1600/PC-1600-Memory-Bank-Switching.md Part 2/4: at 8000-BFFF, page-C bank
    // 0/1 route to Slot 1 (RAM2#), bank 2/3 to Slot 2 (RAM1#). The Port 28H
    // vertical bank is NOT gated here -- a vertical-bank-aware card
    // (CE-1601M) tracks it via its own latch (see ioWrite()); a plain RAM
    // card simply aliases across vertical banks, which is what a module
    // that doesn't decode Port 28H really does.
    bool chipSelect(uint16_t addr) const {
        if (addr < 0x8000 || addr >= 0xC000) return false;
        return ownsBank(m_bank.pageCBank());
    }
    bool ownsBank(int bank) const {
        if (m_slot == Slot::Slot1) return bank == 0 || bank == 1;
        return bank == 2 || bank == 3;
    }
    bool servesBank(int bank, uint16_t addr) const {
        return !m_card.empty() && ownsBank(bank) && addr >= 0x8000 && addr < 0xC000;
    }
    // PinState for a gate-array remap access (readRemapped/writeRemapped):
    // only the two pins a Slot-2 RAM module actually decodes -- pin 4
    // (RAM1# chip select) and pin 5 (PVOUT / A14 half-select) -- plus the
    // 14-bit offset. The Port-31H-derived PT/PU pins and Slot-1 S1/S2/S3
    // sub-selects don't apply when the mainboard has forced the select.
    static PinState remapPins(uint16_t offset, bool pvoutHigh, bool forWrite) {
        PinState p;
        p.address = static_cast<uint16_t>(offset & 0x3FFF);
        p.forWrite = forWrite;
        p.pin[4] = true;        // RAM1#/RAM2# chip select asserted
        p.pin[5] = pvoutHigh;   // PVOUT / SRAM A14
        return p;
    }

    PinState decode(uint16_t addr, bool forWrite) const { return decode(addr, forWrite, m_bank.pageCBank()); }
    PinState decode(uint16_t addr, bool forWrite, int bank) const {
        PinState p;
        p.address = addr;
        p.forWrite = forWrite;

        // PT/PU/PVOUT (pins 19/3/5) carry the accessed page's bank number,
        // broadcast machine-wide (PC-1600 TRM Systemhandbuch §7.2.1's
        // Port 31H table: for an 8000-BFFF access PT:PU:PVOUT == b6:b5:b4).
        // For a slot access that is the page-C bank number -- 000 = Slot 1a,
        // 001 = Slot 1b, so PVOUT (b4) is the 16KB half-select.
        p.pin[19] = (bank & 4) != 0; // PT    = Port 31H b6
        p.pin[3]  = (bank & 2) != 0; // PU    = Port 31H b5
        p.pin[5]  = (bank & 1) != 0; // PVOUT = Port 31H b4

        // Only reached via selected() (read()/write() gate on it), so this
        // slot's chip select is asserted by definition.
        p.pin[4] = true; // RAM2# (Slot 1) / RAM1# (Slot 2) chip select

        if (m_slot == Slot::Slot1 && bank == 0) {
            // Pins 16/17/18 carry S1/S2/S3, the gate array's buffered copies
            // of the SC7852's LHS strobes. They select 2KB blocks of bank 0
            // only, remapped by Port 3CH b6 (TRM SC7852 pins 46-48), and
            // reach the connector in reverse order: pin 16 = LHS3, pin 18 =
            // LHS1 (measured, docs/background/Decisions.md).
            //   b6=0 (boot probe 1AH, CE-151): 16 B800, 17 B000, 18 A800
            //   b6=1 (boot probe 5BH, CE-155): 16 A000, 17 A800, 18 B000
            // A CE-151 (S1+S2) thus tiles B000-BFFF; a CE-155 tiles
            // A000-B7FF and its own Y7 decoder adds B800-BFFF.
            const uint16_t block = addr & 0xF800;
            if (m_bank.lhsRemapped()) {
                p.pin[16] = block == 0xA000;
                p.pin[17] = block == 0xA800;
                p.pin[18] = block == 0xB000;
            } else {
                p.pin[16] = block == 0xB800;
                p.pin[17] = block == 0xB000;
                p.pin[18] = block == 0xA800;
            }
        }
        // Slot 2's pins 16-18 are K0-K2 (I/O-select for Port 28H vertical
        // banking) -- left deasserted, and a CE-1601M-class module does not
        // need them modelled: its vertical-bank latch samples the Port 28H
        // *data-bus write* instead, forwarded here by PC1600Memory::writeIO
        // -> ioWrite() as an I/O-space PinState. Pin 2 (TRM: PVIN) is left
        // deasserted too, although a real PC-1600 ties it to VCC on both
        // slots (measured). TODO.md, "Expansion connectors".
        return p;
    }
};
