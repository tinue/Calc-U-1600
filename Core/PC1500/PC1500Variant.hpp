#pragma once

// ── PC-1500 vs PC-1500A model differences ────────────────────────────────
//
// Encodes the two documented, Core-relevant differences between the
// plain PC-1500 and the PC-1500A -- both run the identical system ROM,
// so nothing here is a CPU/ROM difference, only physical-RAM-chip-
// population and connector-pinout differences:
//
//   1. RAM geometry (consumed by PC1500Memory): the PC-1500A
//      populates three HM6116 chips across S0-S2 (6KB contiguous built-in
//      RAM) and a fourth as full, independently-decoded S7 (2KB, giving it
//      the "machine language area" at &7C01-&7FFF). The plain PC-1500 only
//      populates S0 (one HM6116, 2KB) plus a TC5514 pair at S7 that only
//      decodes 10 of the 2KB window's 11 address lines, so &7C00-&7FFF
//      aliases &7800-&7BFF at a fixed &400 offset (§2.3/§4.2).
//   2. Expansion-connector pin reassignment: four of the 40 connector pins
//      carry a different S-block strobe depending on which model they're
//      plugged into. Encoded in
//      Core/Connector/ExpansionConnector.hpp's routesSBlock() -- the
//      60-pin SystemBus has no equivalent per-variant gap (see that
//      class's own doc comment).
enum class PC1500Variant {
    PC1500,  // plain/non-A: A01, A03, or A04 ROM
    PC1500A, // A04 ROM only
};

// A03/A04 also run unmodified on a plain PC-1500 (A04 was originally
// developed for the PC-1500A's launch but was later also used in
// later-production PC-1500 units); A01/A03 are untested on a PC-1500A and
// must not be offered for it.
enum class PC1500RomRevision { A01, A03, A04 };
