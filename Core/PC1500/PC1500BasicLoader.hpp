#pragma once
#include <cstdint>
#include <string>
#include <vector>

class PC1500Machine;

// ── Fast BASIC program loading for the PC-1500 / PC-1500A ───────────────
//
// The keystroke typer (PC1500BasicTyper) drives the ROM's line editor one
// character at a time -- exact, but minutes of emulated time for a real
// program. This path takes an already-tokenized program (a SharpDataExchange
// CE-158 transfer file, parsed by Core/Basic/BasicBinaryImage), pokes the
// payload straight into the BASIC program area, appends the 0xFF program-end
// marker, and writes BASPRG_END.
//
// It does NOT run NEW0 -- the preset is responsible for leaving the machine
// in a clean, loadable state first (`- key: cl` / `- type: NEW0`), the same
// contract `format: basic-text` already has. NEW0 is what initialises
// BASPRG_ST / VAR_START (both $FFFF after a bare boot) and $FF-fills the
// program area.
//
// Layout facts (cross-checked with tools/pc1500_cli --dump-basic against the
// keystroke typer): the in-RAM line records are byte-identical to the
// transfer-file payload, the program ends with one 0xFF at BASPRG_END
// ($7867, big-endian), and BASPRG_ST ($7865) is the load address (stock
// $40C5, lower with a low-window RAM module).

struct PC1500BasicLoadResult {
    bool ok = false;
    uint16_t baseAddr = 0;  // BASPRG_ST after NEW0 (where the payload was written)
    uint16_t endAddr = 0;   // BASPRG_END after the load (the 0xFF marker)
    std::string error;      // set when ok == false
};

/// Loads a pre-tokenized PC-1500 BASIC program (`transferFile` = a full
/// CE-158 transfer file, header included) into `machine`. The machine must
/// be booted, at the BASIC prompt, and NEW0'd by the preset -- this call
/// does not type anything. Used by the GUI `-loadBasicBinary:` action.
PC1500BasicLoadResult loadBasicBinaryProgram(PC1500Machine& machine,
                                             const std::vector<uint8_t>& transferFile);

/// Same, but takes the bare tokenized payload (no CE-158 header) -- the run
/// of in-RAM line records. Used by the preset loader, which tokenizes a
/// `.bas` listing headerless via libsharpdx.
PC1500BasicLoadResult loadBasicBinaryPayload(PC1500Machine& machine,
                                             const std::vector<uint8_t>& payload);
