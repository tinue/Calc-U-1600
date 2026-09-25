#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../Basic/BasicLoadResults.hpp"

class PC1600Machine;

// ── Fast BASIC program loading for the PC-1600 ─────────────────────────
//
// The PC-1600 analog of Core/PC1500/PC1500BasicLoader: takes an
// already-tokenized program (a SharpDataExchange PC-1600 transfer file,
// parsed by Core/Basic/BasicBinaryImage), pokes the payload into the BASIC
// program area, appends the 0xFF end marker, and writes BASPRG_END --
// instead of typing the program in character by character.
//
// It does NOT type anything and never resets the machine or touches MODE:
// this is LOAD semantics, not NEW+type. It works off whatever
// BASPRG_ST/BASPRG_END are currently live -- validates both are plausible,
// erases the resident program between them, pokes the new payload in from
// BASPRG_ST, and fixes up BASPRG_END. The caller is responsible for having
// prepared the machine first (memory cards, `NEW`, mode) exactly as on real
// hardware; a preset's own `- type: NEW0` step (the same contract
// `format: basic-text` has) works fine too, since a freshly-NEW0'd program
// is zero-length and the erase step is then a no-op.
//
// Layout facts (verifiable with tools/pc1600_cli --preset ... --dump-basic
// against the keystroke typer): the in-RAM line records are
// byte-identical to the transfer-file payload; the program ends with a
// single 0xFF at BASPRG_END. Pointer quirks vs. the PC-1500:
//
//   * The write address is the little-endian Z-80-native pointer at $F5CF
//     (stock $C0C5; $80C5 with a Slot-1/Slot-2 RAM module -- it tracks
//     wherever `NEW` put the program area, with the bank registers parked
//     there). BASPRG_ST/END/EDT ($F865/$F867/$F869) hold *LH5803-side*
//     addresses whose absolute value depends on the bank window in use
//     ($40C5 stock, $00C5 with the program in a switched bank) -- $F865 is
//     read only to form BASPRG_END in the same representation.
//   * BASPRG_END ($F867) is big-endian, written as $F865's value + length.

/// `transferFile` = a full PC-1600 transfer file, header included. Requires
/// only that BASPRG_ST/BASPRG_END are currently valid pointers -- no reset,
/// mode change, or NEW0 is performed. Nothing in the app calls this today --
/// the GUI and presets load `.bas` listings (loadBasicBinaryPayload() below);
/// it stays for already-tokenized transfer files and is covered by the tests.
BasicLoadResult loadBasicBinaryProgram(PC1600Machine& machine,
                                       const std::vector<uint8_t>& transferFile);

/// Same, but takes the bare tokenized payload (no header). Used by the
/// preset runner (`format: basic-binary`) and the GUI's Load BASIC Program,
/// which both tokenize a `.bas` listing headerless via
/// basic::readBasicProgramSource().
BasicLoadResult loadBasicBinaryPayload(PC1600Machine& machine,
                                       const std::vector<uint8_t>& payload);
