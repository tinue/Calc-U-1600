#pragma once
#include <string>
#include <vector>

#include "Inspector.hpp"

class PC1500Machine;

// ── PC-1500 / PC-1500A inspector views (Inspector.hpp) ───────────────────
//
// Run under the machine lock, like the PC-1600's:
//
//   machine.debugInspect([&](const PC1500Machine& m) { return inspect::pc1500View(m, v); });
//
// Sources: Ref/PC-1500/Memory-Architecture/PC-1500-BASIC-Pointers.md and
// PC-1500-Address-Decoding.md; names as in the ROM disassembly's
// PC-1500.lib, CE-150.lib and CE-158.lib.
namespace inspect {

std::vector<std::string> pc1500View(const PC1500Machine& m, View v);

/// The Memory ▾ (`dumps` false) or Dump ▾ (`dumps` true) entries.
std::vector<MenuEntry> pc1500Menu(const PC1500Machine& m, bool dumps);

}  // namespace inspect
