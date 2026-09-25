#include "ProgramLoader.hpp"

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
    if (!file.ok && !file.lengthMismatch) {
        r.error = file.error;
        return r;
    }
    const machinecode::Target kind = pc1600 ? machinecode::Target::PC1600 : machinecode::Target::PC1500;
    const std::string mismatch = machinecode::headerMismatch(kind, file);
    if (!mismatch.empty()) {
        r.error = mismatch;
        return r;
    }
    if (file.payload.empty()) {
        r.error = req.bin + " is empty";
        return r;
    }
    uint32_t addr = req.hasAddress ? req.address : file.loadAddr;
    if (!req.hasAddress && file.header == machinecode::File::Header::None) {
        r.error = "a headerless program needs an \"address\"";
        return r;
    }
    const size_t len = file.payload.size();
    if (addr + len > 0x10000) {
        r.error = "the program doesn't fit below 0x10000";
        return r;
    }

    std::string err;
    machinecode::Slot slot = machinecode::Slot::S0;
    uint32_t z80Addr = addr; // where the bytes go on the Z-80 side (PC-1600)
    if (pc1500) {
        if (!loadPC1500MachineCode(*pc1500, addr, file.payload.data(), len, &err)) {
            r.error = err;
            return r;
        }
    } else {
        // The LH5803 sees the Z-80's 8000-FFFF at its 0000-7FFF.
        if (req.thread == 2) {
            if (addr + len > 0x8000) {
                r.error = "LH5803 code must sit in 0000-7FFF (the Z-80's 8000-FFFF)";
                return r;
            }
            z80Addr = addr + 0x8000;
        }
        if (req.slot >= 0) {
            slot = machinecode::Slot(req.slot);
        } else {
            const std::vector<machinecode::BasicArea> areas = pc1600BasicAreas(*pc1600);
            std::string why;
            if (!machinecode::pc1600TargetFor(z80Addr, len, areas, &slot, &why)) {
                if (z80Addr >= 0xC000) slot = machinecode::Slot::S0; // internal RAM: always writable
                else {
                    r.error = why;
                    return r;
                }
            }
        }
        if (!loadPC1600MachineCode(*pc1600, int(slot), z80Addr, file.payload.data(), len, &err)) {
            r.error = err;
            return r;
        }
    }
    r.lo = uint16_t(addr);
    r.hi = uint16_t(addr + len - 1);
    r.entry = req.hasEntry ? req.entry : uint16_t(file.autorunAddr ? file.autorunAddr : addr);

    // How BASIC starts it.
    uint32_t ramStart = 0, ramEnd = 0;
    if (pc1500) pc1500UserRam(*pc1500, &ramStart, &ramEnd);
    const std::vector<machinecode::BasicArea> areas = pc1600 ? pc1600BasicAreas(*pc1600) : std::vector<machinecode::BasicArea>{};
    if (req.thread == 2) {
        r.callCommand = ""; // LH5803 code is entered from Z-80 code (CALLH), not from BASIC
    } else {
        // advice() starts at r.entry (the `entry` override, the header's
        // auto-run address, or the load address) and adds slot 2's bank.
        r.callCommand = machinecode::advice(kind, slot, pc1600 ? z80Addr : addr, len, r.entry, ramStart, ramEnd, areas).callCommand;
    }

    if (!req.listing.empty()) {
        Listing listing;
        if (!loadListing(req.listing, &listing, &err, req.source)) {
            r.warnings.push_back("listing not loaded: " + err);
        } else {
            for (const std::string& w : listing.warnings) r.warnings.push_back(req.listing + ": " + w);
            for (const std::string& path : req.symbols) {
                Listing symbols;
                if (loadSymbolFile(path, &symbols, &err))
                    listing.symbols.insert(symbols.symbols.begin(), symbols.symbols.end());
                else
                    r.warnings.push_back(err);
            }
            r.binding = map.addLoaded(req.thread, std::move(listing), req.key, r.lo, r.hi, req.listing);
            const int bad = map.verify(
                r.binding, [&target](int t, const BankKey& k, uint16_t a) { return target.bankMatches(t, k, a); },
                [&target](int t, uint16_t a, uint8_t* v) { return target.peek(t, kSpaceMain, a, v); });
            if (bad > 0)
                r.warnings.push_back(std::to_string(bad) + " listing lines don't match the loaded bytes -- is the listing from this build?");
        }
    }
    r.ok = true;
    return r;
}

} // namespace debug
