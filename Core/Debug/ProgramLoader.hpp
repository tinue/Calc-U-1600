#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "DebugTarget.hpp"
#include "SourceMap.hpp"

class PC1500Machine;
class PC1600Machine;

// ── Build & Load ─────────────────────────────────────────────────────────
//
// Puts a freshly assembled program into the machine through the same
// machine-code loaders as "Load Machine Code…" (no dialog: the address
// comes from the request or the file's header) and binds its listing to
// exactly the loaded range, replacing whatever listing an earlier load of
// that range brought. That is how the debugger knows which listing
// belongs to which bytes.

namespace debug {

struct LoadRequest {
    std::string bin;                  ///< the program (.bin, headerless or with a CE-158 / PC-1600 header)
    std::string listing;              ///< its listing (.lst / .rst); empty = none
    std::string source;               ///< the listing's main source, if not derivable
    std::vector<std::string> symbols; ///< extra .SYMBOLS: files
    int thread = 1;                   ///< the CPU it's for (PC-1600: 1 = Z-80, 2 = LH5803)
    BankKey key;
    bool hasAddress = false;          ///< else the header's, else (headerless) the listing's lowest address
    uint32_t address = 0;             ///< in `thread`'s address space
    int slot = -1;                    ///< PC-1600 Z-80: 0 = S0, 1 / 2; -1 = from the address
    bool hasEntry = false;            ///< else entrySymbol, else the header's autorun address, else the
    uint16_t entry = 0;               ///< listing's ENTRY (if in the loaded range), else the load address
    std::string entrySymbol;          ///< a symbol of the listing, or an address as text
};

struct LoadResult {
    bool ok = false;
    std::string error;
    uint16_t lo = 0, hi = 0;          ///< the loaded range, in `thread`'s address space
    uint16_t entry = 0;
    std::string callCommand;          ///< the BASIC command that runs it, e.g. "CALL &40C5"
    int binding = 0;                  ///< the SourceMap binding of its listing (0 = no listing)
    std::vector<std::string> warnings;
};

/// Exactly one of the machines is non-null. Verifies the bound listing
/// against memory right away (`target` supplies the peek and bank state).
LoadResult loadProgram(PC1500Machine* pc1500, PC1600Machine* pc1600, const LoadRequest& request, SourceMap& map,
                       const DebugTarget& target);

} // namespace debug
