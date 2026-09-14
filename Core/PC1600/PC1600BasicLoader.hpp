#pragma once
#include <cstdint>
#include <string>
#include <vector>

class PC1600Machine;

// ── Fast BASIC program loading for the PC-1600 ─────────────────────────
//
// The PC-1600 analog of Core/PC1500/PC1500BasicLoader: takes an
// already-tokenized program (a SharpDataExchange PC-1600 transfer file,
// parsed by Core/Basic/BasicBinaryImage), pokes the payload into the BASIC
// program area, appends the 0xFF end marker, and writes BASPRG_END --
// instead of typing the program in character by character.
//
// It does NOT type anything: the preset must first put the machine in PRO
// mode and run NEW0 (`- key: mode` / `- type: NEW0`), the same contract
// `format: basic-text` has.
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

struct PC1600BasicLoadResult {
    bool ok = false;
    uint16_t baseAddr = 0;  // SC7852-side base the payload was written to (e.g. $C0C5)
    uint16_t endAddr = 0;   // SC7852-side address of the 0xFF end marker
    std::string error;
};

/// `transferFile` = a full PC-1600 transfer file, header included. Used by
/// the GUI `-loadBasicBinary:` action.
PC1600BasicLoadResult loadBasicBinaryProgram(PC1600Machine& machine,
                                             const std::vector<uint8_t>& transferFile);

/// Same, but takes the bare tokenized payload (no header). Used by the
/// preset loader, which tokenizes a `.bas` listing headerless via libsharpdx.
PC1600BasicLoadResult loadBasicBinaryPayload(PC1600Machine& machine,
                                             const std::vector<uint8_t>& payload);
