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
// It does NOT run NEW0 and never resets the machine: this is LOAD semantics,
// not NEW+type. It works off whatever BASPRG_ST/BASPRG_END are currently
// live -- validates both are plausible, erases the resident program between
// them, pokes the new payload in from BASPRG_ST, and fixes up BASPRG_END. The
// caller is responsible for having prepared the machine first (memory cards,
// `NEW`, mode) exactly as on real hardware; a preset's own `- type: NEW0`
// step (the same contract `format: basic-text` has) works fine too, since a
// freshly-NEW0'd program is zero-length and the erase step is then a no-op.
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
/// CE-158 transfer file, header included) into `machine`. Requires only that
/// BASPRG_ST/BASPRG_END are currently valid pointers -- no reset, mode
/// change, or NEW0 is performed. Used by the GUI `-loadBasicBinary:` action.
PC1500BasicLoadResult loadBasicBinaryProgram(PC1500Machine& machine,
                                             const std::vector<uint8_t>& transferFile);

/// Same, but takes the bare tokenized payload (no CE-158 header) -- the run
/// of in-RAM line records. Used by the preset loader, which tokenizes a
/// `.bas` listing headerless via libsharpdx.
PC1500BasicLoadResult loadBasicBinaryPayload(PC1500Machine& machine,
                                             const std::vector<uint8_t>& payload);
