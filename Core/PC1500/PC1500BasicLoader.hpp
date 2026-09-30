#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../Basic/BasicLoadResults.hpp"

class PC1500Machine;

// ── Fast BASIC program loading for the PC-1500 / PC-1500A ───────────────
//
// The keystroke typer (PC1500BasicTyper) drives the ROM's line editor one
// character at a time -- exact, but minutes of emulated time for a real
// program. This path takes a BASIC listing (tokenized in-process) or an
// already-tokenized CE-158 file (basic::readBasicProgram()), pokes the
// payload straight into the BASIC program area, appends the 0xFF program-end
// marker, and writes BASPRG_END.
//
// It does NOT run NEW0 and never resets the machine: this is LOAD semantics,
// not NEW+type. It works off whatever BASPRG_ST/BASPRG_END are currently
// live -- validates both are plausible, erases the resident program between
// them, pokes the new payload in from BASPRG_ST, and fixes up BASPRG_END. The
// caller is responsible for having prepared the machine first (memory cards,
// `NEW`, mode) exactly as on real hardware; a preset's own `- type: NEW0`
// step (the same contract a typed `program:` has) works fine too, since a
// freshly-NEW0'd program is zero-length and the erase step is then a no-op.
//
// Layout facts (cross-checked with tools/pc1500_cli --dump-basic against the
// keystroke typer): the in-RAM line records are byte-identical to the
// transfer-file payload, the program ends with one 0xFF at BASPRG_END
// ($7867, big-endian), and BASPRG_ST ($7865) is the load address (stock
// $40C5, lower with a low-window RAM module).

/// Loads a BASIC program into `machine`: `file` is a `.bas` listing or
/// tokenized BASIC behind a CE-158 header (a PC-1600 file is refused).
/// Requires only that BASPRG_ST/BASPRG_END are currently valid pointers --
/// no reset, mode change, or NEW0 is performed.
BasicLoadResult loadBasicProgram(PC1500Machine& machine, const std::vector<uint8_t>& file);

/// Same, for the file at `path`: Load BASIC Program and a
/// preset's BASIC `program: file:`.
BasicLoadResult loadBasicProgramFile(PC1500Machine& machine, const std::string& path);

/// The bare tokenized payload (no header) -- the run of in-RAM line records
/// basic::readBasicProgram() yields.
BasicLoadResult loadBasicBinaryPayload(PC1500Machine& machine,
                                       const std::vector<uint8_t>& payload);
