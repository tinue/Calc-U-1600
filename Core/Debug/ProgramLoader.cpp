#include "ProgramLoader.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

#include "../MachineCodeFile.hpp"
#include "../PC1500/PC1500Machine.hpp"
#include "../PC1500/PC1500MachineCodeLoader.hpp"
#include "../PC1600/PC1600Machine.hpp"
#include "../PC1600/PC1600MachineCodeLoader.hpp"
#include "Listing/Listing.hpp"

namespace debug {

LoadResult loadProgram(PC1500Machine* pc1500, PC1600Machine* pc1600, const LoadRequest& req, SourceMap& map,
                       const DebugTarget& target) {
    LoadResult r;
    std::ifstream in(req.bin, std::ios::binary);
    if (!in) {
        r.error = "can't read " + req.bin;
        return r;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const machinecode::File file = machinecode::readFile(bytes);

    // The listing first: a headerless file without an "address" loads at the
    // listing's lowest address (the source's .org), so one launch
    // configuration serves any program.
    Listing listing;
    const bool haveListing = (!req.listing.empty() || !req.symbols.empty()) &&
                             loadListingWithSymbols(req.listing, req.source, req.symbols, &listing, &r.warnings);
    bool hasAddress = req.hasAddress;
    uint32_t address = req.address;
    if (!hasAddress && file.header == machinecode::File::Header::None && haveListing && !listing.lines.empty()) {
        address = listing.lines.front().addr;
        for (const ListingLine& l : listing.lines) address = std::min<uint32_t>(address, l.addr);
        hasAddress = true;
    }

    // Build & Load's rules: a header length mismatch still loads, the
    // request's address and slot override, LH5803 code goes to the Z-80's
    // 8000-FFFF, and a PC-1600 slot is otherwise the BASIC area's (internal
    // RAM always).
    machinecode::LoadOptions options;
    options.target = pc1600 ? machinecode::Target::PC1600 : machinecode::Target::PC1500;
    options.acceptLengthMismatch = true;
    options.hasAddress = hasAddress;
    options.address = address;
    options.lh5803 = pc1600 && req.thread == 2;
    if (req.slot >= 0) options.slot = machinecode::Slot(req.slot);
    else options.slotPolicy = machinecode::SlotPolicy::DeriveOrInternal;
    const std::vector<machinecode::BasicArea> areas = pc1600 ? pc1600BasicAreas(*pc1600) : std::vector<machinecode::BasicArea>{};
    const machinecode::LoadPlan plan = machinecode::planLoad(file, options, areas);
    switch (plan.error) {
        case machinecode::LoadError::None: break;
        case machinecode::LoadError::Empty: r.error = req.bin + " is empty"; return r;
        case machinecode::LoadError::NeedsAddress: r.error = "a headerless program needs an \"address\" or a listing"; return r;
        case machinecode::LoadError::OutsideBank0:
        case machinecode::LoadError::PastEnd: r.error = "the program doesn't fit below 0x10000"; return r;
        case machinecode::LoadError::LhRange: r.error = "LH5803 code must sit in 0000-7FFF (the Z-80's 8000-FFFF)"; return r;
        default: r.error = plan.detail; return r; // BadFile, HeaderMismatch, NoSlot
    }

    std::string err;
    const bool written = pc1500 ? loadPC1500MachineCode(*pc1500, plan.busAddr, file.payload.data(), plan.len, &err)
                                : loadPC1600MachineCode(*pc1600, int(plan.slot), plan.busAddr, file.payload.data(), plan.len, &err);
    if (!written) {
        r.error = err;
        return r;
    }
    const uint32_t addr = plan.addr;
    r.lo = uint16_t(addr);
    r.hi = uint16_t(addr + plan.len - 1);
    // The entry: the request's (an address, or a symbol of the listing --
    // a name the listing defines wins over reading it as hex), else the
    // header's auto-run address, else the listing's ENTRY if it lies in the
    // loaded range, else the load address.
    const auto symbol = [&](const std::string& name, uint16_t* v) {
        const auto it = haveListing ? listing.symbols.find(name) : listing.symbols.end();
        if (it == listing.symbols.end()) return false;
        *v = it->second;
        return true;
    };
    uint16_t entry = 0;
    uint32_t parsed = 0;
    if (req.hasEntry) {
        r.entry = req.entry;
    } else if (!req.entrySymbol.empty()) {
        if (symbol(req.entrySymbol, &entry)) r.entry = entry;
        else if (machinecode::parseHexAddress(req.entrySymbol, &parsed)) r.entry = uint16_t(parsed);
        else {
            r.error = "\"entry\": " + req.entrySymbol + " is neither a symbol of the listing nor an address";
            return r;
        }
    } else if (file.autorunAddr) {
        r.entry = uint16_t(file.autorunAddr);
    } else if (symbol("ENTRY", &entry) && entry >= r.lo && entry <= r.hi) {
        r.entry = entry;
    } else {
        if (symbol("ENTRY", &entry))
            r.warnings.push_back("ENTRY is outside the loaded range, so the program starts at its load address");
        r.entry = r.lo;
    }

    // How BASIC starts it.
    uint32_t ramStart = 0, ramEnd = 0;
    if (pc1500) pc1500UserRam(*pc1500, &ramStart, &ramEnd);
    if (req.thread == 2) {
        r.callCommand = ""; // LH5803 code is entered from Z-80 code (CALLH), not from BASIC
    } else {
        // advice() starts at r.entry (the `entry` override, the header's
        // auto-run address, or the load address) and adds slot 2's bank.
        r.callCommand = machinecode::advice(options.target, plan.slot, plan.busAddr, plan.len, r.entry, ramStart, ramEnd, areas).callCommand;
    }

    // The listing and symbols bind to the loaded range; symbols alone too.
    if (haveListing) {
        const std::string& name = req.listing.empty() ? req.symbols.front() : req.listing;
        r.binding = map.addLoaded(req.thread, std::move(listing), req.key, r.lo, r.hi, name);
        const int bad = map.verify(
            r.binding, [&target](int t, const BankKey& k, uint16_t a) { return target.bankMatches(t, k, a); },
            [&target](int t, uint16_t a, uint8_t* v) { return target.peek(t, kSpaceMain, a, v); });
        if (bad > 0)
            r.warnings.push_back(std::to_string(bad) + " listing lines don't match the loaded bytes -- is the listing from this build?");
    }
    r.ok = true;
    return r;
}

} // namespace debug
