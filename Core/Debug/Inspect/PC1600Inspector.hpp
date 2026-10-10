#pragma once
#include <string>
#include <vector>

#include "Inspector.hpp"

class PC1600Machine;

// ── PC-1600 inspector views (Inspector.hpp) ──────────────────────────────
//
// Both calls read the machine through its const accessors only, so they
// must run under the machine lock:
//
//   machine.debugInspect([&](const PC1600Machine& m) { return inspect::pc1600View(m, v); });
//
// Sources: Ref/PC-1600/PC-1600-Work-Area-Map.md (the work area, ADTBL, MEM
// and STATUS formulas), PC-1600-Memory-Architecture.md (program areas,
// expansion vs. program memory vs. RAM disk), PC-1600-Filesystem.md §4-5
// (RAM-disk layout and boot sector); names as in the ROM disassembly's
// symbol tables.
namespace inspect {

std::vector<std::string> pc1600View(const PC1600Machine& m, View v);

/// The Memory ▾ (`dumps` false) or Dump ▾ (`dumps` true) entries.
std::vector<MenuEntry> pc1600Menu(const PC1600Machine& m, bool dumps);

}  // namespace inspect
