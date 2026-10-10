#pragma once
#include <string>
#include <vector>

// ── The debug panel's inspector views ────────────────────────────────────
//
// INSPECTOR (Qt6's DebugPanel) shows the machine's state as text: the
// work-area pointers, tables of what memory exists and where each CPU sees
// it, and hex dumps of the areas BASIC and the file system use. The views
// are built here in Core, from one locked snapshot of the machine
// (PC1500Machine/PC1600Machine::debugInspect()), so they are unit-tested
// and every read is free of side effects. PC1500Inspector.hpp /
// PC1600Inspector.hpp build them; this header names them.
namespace inspect {

enum class View {
    Pointers,
    // Memory ▾
    Inventory,     // every ROM / RAM present, built in and plugged in
    Z80View,       // PC-1600: the SC7852's pages A-D, live
    LhView,        // the LH5801 (PC-1500) / LH5803 (PC-1600) address space, live
    BasicArea,     // the BASIC area (S0), bottom to top, with its pointers
    ProgramAreas,  // PC-1600: S0/S1/S2 (TITLE)
    DiskAreas,     // PC-1600: the RAM disks S1:/S2:
    PhysicalRam,   // each RAM chip: which part holds what
    // Dump ▾
    DumpBasicArea,
    DumpProgramS1, // PC-1600: a program module's area (S0 is DumpBasicArea)
    DumpProgramS2,
    DumpDiskS1,    // PC-1600: a RAM disk, every byte
    DumpDiskS2,
    DumpMlArea,    // PC-1500: the machine-language area 7C00H-7FFFH
};

/// The menu text for a view; `pc1500` names the PC-1500's CPU (LH5801).
const char* viewTitle(View v, bool pc1500 = false);

/// A view's short name for the CLIs' `--inspect` ("pointers", "z80",
/// "dump-disk-s2", ...), and back; false for an unknown name.
const char* viewName(View v);
bool viewFromName(const std::string& name, View* v);

/// One menu entry: the view and whether it applies to the machine right now
/// (a module in that slot, a RAM disk, ...). A view a model doesn't have at
/// all is left out of the list.
struct MenuEntry {
    View view;
    bool enabled;
};

}  // namespace inspect
