#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Result types shared by the per-machine BASIC loaders: the keystroke typers
// (PC1500BasicTyper / PC1600BasicTyper, `format: basic-text`) and the fast
// tokenized loaders (PC1500BasicLoader / PC1600BasicLoader,
// `format: basic-binary` and the GUI's Load BASIC Program).

/// typeBasicProgramText()'s outcome.
struct BasicTypeResult {
    bool ok = false;
    /// Source lines the machine's line editor didn't store -- rejected by the
    /// ROM, or (PC-1600) over-length and never typed. Non-empty implies
    /// `ok == false`.
    std::vector<std::string> rejectedLines;
    std::string error;  // set when ok is false
};

/// loadBasicBinaryProgram() / loadBasicBinaryPayload()'s outcome. Addresses
/// are in the main CPU's address space (LH5801 on the PC-1500, SC7852 on the
/// PC-1600).
struct BasicLoadResult {
    bool ok = false;
    uint16_t baseAddr = 0;  // where the payload was written (the program start)
    uint16_t endAddr = 0;   // the 0xFF program-end marker
    std::string error;      // set when ok == false
};
