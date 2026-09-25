#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Shared card interface + pin-state type for the 40-pin connector family
// (ExpansionConnector on the PC-1500/1500A, MemorySlotConnector on the
// PC-1600) and the 60-pin SystemBus.
//
// The governing principle (docs/Memory-Card-Definition-Spec.md §1/§4, and
// the project owner's own framing): a real module plugged into a slot wires
// contact-for-contact -- pin 1 to pin 1, pin 2 to pin 2 -- and has no idea
// what the host calls those pins. So a card decodes purely from `pin[]`,
// the physical 40-pin edge-connector contacts, and never asks which host or
// which slot it is in. Each *connector* is what knows the host: it drives
// each physical pin with whatever signal its own TRM pin table says that
// contact carries (Y0/Y2/S1-S5 on the PC-1500; RAM1/RAM2, PVOUT, PU, PT,
// K0-K2/S1-S3, INH on the PC-1600 -- see MemorySlotConnector).
struct PinState {
    uint16_t address = 0;
    bool forWrite = false;

    // Physical 40-pin edge-connector contacts, 1-indexed (index 0 unused).
    // Only the signal/strobe pins are ever set by a connector; the address
    // bus (22-37), data bus (7-14) and RD/WR (38/39) are carried by
    // `address`, the read return value, and `forWrite` respectively.
    //
    // Pin roles per host (a card must not care which):
    //   pin[2]  PC-1500 PV        / PC-1600 PVIN
    //   pin[3]  PC-1500 PU        / PC-1600 PU
    //   pin[4]  PC-1500 Y0 (CS &0000-&3FFF) / PC-1600 RAM2 (Slot 1) or RAM1 (Slot 2) CS
    //   pin[5]  PC-1500 S4        / PC-1600 PVOUT
    //   pin[6]  PC-1500 DME0      / PC-1600 MREQ
    //   pin[15] INHIBIT / INH (see InhibitSource -- output from the card)
    //   pin[16] PC-1500 S1 (PC-1500A S3) / PC-1600 S1 (Slot 1) or K0 (Slot 2)
    //   pin[17] PC-1500 S2 (PC-1500A S4) / PC-1600 S2 (Slot 1) or K1 (Slot 2)
    //   pin[18] PC-1500 S3 (PC-1500A S5) / PC-1600 S3 (Slot 1) or K2 (Slot 2)
    //   pin[19] PC-1500 Y2 (CS &8000-&BFFF) / PC-1600 PT
    bool pin[41] = {};

    // True only for an access SystemBus routed via its ME1 path (DME1/ME1,
    // pins 58/59 -- the 40-pin connectors have no equivalent). ME1's own
    // sub-decode into named chip-select blocks isn't documented/modeled, so
    // a card wanting ME1 space must key off `me1 && address` directly.
    bool me1 = false;

    // True when this write originates from a host poke() -- the
    // debug/preset-loader path -- rather than a guest-CPU store. A card that
    // gates runtime writes (e.g. the CE-163F's flash banks, which otherwise
    // need a JEDEC unlock sequence) treats a direct write as an
    // unconditional array write. Always false for reads and for guest-CPU
    // writes.
    bool direct = false;

    // True when this is an I/O-space write (an `OUT (n),A` on the PC-1600),
    // not a memory access. `address & 0xFF` is the port number and the
    // `respondsToWrite` value is the data bus byte. Used by a card whose
    // bank latch triggers on a port write (CE-1601M: `OUT (28H)`). The
    // pin[] chip-selects are not meaningful for an I/O write. Only
    // MemorySlotConnector routes these today (Slot 2's 28-2FH range).
    bool ioWrite = false;
};

// What a card did with a write. Converts to bool as "claimed" (the bus
// stops looking for another responder), which is all a guest-CPU store
// needs; a host poke() also asks whether the byte was actually stored.
struct WriteResult {
    bool claimed = false;
    bool stored = false;

    /// Not this card's access.
    static constexpr WriteResult ignored() { return {false, false}; }
    /// Claimed, but the value went nowhere (mask ROM, write-protected
    /// RAM, a read-only register).
    static constexpr WriteResult refused() { return {true, false}; }
    /// Claimed and acted on (stored, or latched into a register).
    static constexpr WriteResult taken() { return {true, true}; }

    constexpr operator bool() const { return claimed; }
};

// A card that can drive INHIBIT (Expansion-Connectors.md's INHIBIT/INH
// pin, pin 15) to suppress the host's internal ROM and substitute its own
// content derives from this too. Connectors look for it once, when the
// card is attached, so the (usual) cards without it cost nothing on each
// host-ROM fetch -- and a card can't answer assertsInhibit() without
// declaring the capability. Polarity-free at this interface: "true =
// suppress ROM"; the connector applies the host's electrical polarity (the
// PC-1500 pulls the pin low, the PC-1600 drives it high).
class InhibitSource {
public:
    virtual ~InhibitSource() = default;
    virtual bool assertsInhibit() const = 0;
};

class ExpansionCard {
public:
    virtual ~ExpansionCard() = default;

    /// Return true and set outValue if this card responds to this access.
    /// Returning false leaves the bus open (0xFF).
    virtual bool respondsToRead(const PinState& pins, uint8_t& outValue) const = 0;

    /// Whether this card claims this write, and whether it stored it
    /// (see WriteResult).
    virtual WriteResult respondsToWrite(const PinState& pins, uint8_t value) = 0;

    /// The bank index this card currently exposes through its main banked
    /// window, for the GUI debug "Dump Mem" panel's per-column labels.
    /// Return -1 (the default) when the card has no bank concept -- a plain
    /// unbanked RAM chip -- so the dump shows nothing rather than a
    /// misleading "bank 0".
    ///
    /// This is the card's OWN latched state, not the mainboard's Port 28H
    /// vertical-bank value: a module that doesn't decode Port 28H (every
    /// card in this project so far) sits on whatever its trigger-pin latch
    /// last sampled -- which stays bank 0 for the whole life of a PC-1600
    /// Slot 2 card, since that bay's pin 18 carries K2, not the S3 strobe
    /// (see ce1638.card.yaml's Slot 1 caveat). Showing it there is more of a
    /// curiosity than a useful number, but it is the module's real state.
    virtual int debugCurrentBank() const { return -1; }

    /// How many banks the card's main banked window can select between
    /// (`debugCurrentBank()`'s range is 0..debugBankCount()-1), for the GUI
    /// debug view's "N-way paging" resource label. Return -1 (the default)
    /// for a card with no bank concept, same convention as
    /// debugCurrentBank() -- a plain unbanked chip has no page count to show.
    virtual int debugBankCount() const { return -1; }

    /// The card's entire backing store, every bank / vertical bank
    /// concatenated in ascending order, for the GUI debug "Dump Mem"
    /// contents view. Empty (the default) for a card with no readable
    /// storage. The dump chunks this into 16 KB rows; a store that is a
    /// whole number of 16 KB banks (the slot cards) labels them bank 0..N.
    virtual std::vector<uint8_t> debugImage() const { return {}; }

    /// The write counterpart of debugImage(): overwrite `n` bytes of the
    /// backing store starting at concatenated offset `off` (same address
    /// space debugImage() returns). For a host-side debug / program-loader
    /// path that must land bytes in a card's RAM regardless of the current
    /// bank-register / pin state -- e.g. the PC-1600 fast BASIC loader
    /// injecting a tokenised program into a slot-RAM program area the
    /// emulated bus does not currently map for writes. Returns false,
    /// writing nothing, when the card has no writable backing or the range
    /// would run past its end. Default: no writable backing.
    virtual bool debugImageWrite(size_t /*off*/, const uint8_t* /*data*/, size_t /*n*/) {
        return false;
    }

    /// The module's name (a definition's `module-name:`, e.g. "CE-1600M"),
    /// so the GUI can read what sits in a slot from the slot itself.
    /// Empty for a card without one (test stubs).
    virtual std::string moduleName() const { return {}; }
};
