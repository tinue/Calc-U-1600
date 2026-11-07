#include "Inspector.hpp"

#include <string>

namespace inspect {

const char* viewTitle(View v, bool pc1500) {
    switch (v) {
        case View::Pointers: return "Pointers";
        case View::Inventory: return "Inventory";
        case View::Z80View: return "Z80 View";
        case View::LhView: return pc1500 ? "LH5801 View" : "LH5803 View";
        case View::BasicArea: return "BASIC Area";
        case View::ProgramAreas: return "Program Areas";
        case View::DiskAreas: return "Disk Areas";
        case View::PhysicalRam: return "Physical RAM";
        case View::DumpBasicArea: return "BASIC Area";
        case View::DumpProgramS1: return "Program Area S1";
        case View::DumpProgramS2: return "Program Area S2";
        case View::DumpDiskS1: return "Disk S1:";
        case View::DumpDiskS2: return "Disk S2:";
        case View::DumpMlArea: return "Machine-Language Area";
    }
    return "?";
}

namespace {
constexpr View kAllViews[] = {
    View::Pointers,      View::Inventory,     View::Z80View,       View::LhView,     View::BasicArea,
    View::ProgramAreas,  View::DiskAreas,     View::PhysicalRam,   View::DumpBasicArea,
    View::DumpProgramS1, View::DumpProgramS2, View::DumpDiskS1,    View::DumpDiskS2, View::DumpMlArea,
};
}  // namespace

const char* viewName(View v) {
    switch (v) {
        case View::Pointers: return "pointers";
        case View::Inventory: return "inventory";
        case View::Z80View: return "z80";
        case View::LhView: return "lh";
        case View::BasicArea: return "basic";
        case View::ProgramAreas: return "programs";
        case View::DiskAreas: return "disks";
        case View::PhysicalRam: return "physical";
        case View::DumpBasicArea: return "dump-basic";
        case View::DumpProgramS1: return "dump-s1";
        case View::DumpProgramS2: return "dump-s2";
        case View::DumpDiskS1: return "dump-disk-s1";
        case View::DumpDiskS2: return "dump-disk-s2";
        case View::DumpMlArea: return "dump-ml";
    }
    return "?";
}

bool viewFromName(const std::string& name, View* v) {
    for (View x : kAllViews)
        if (name == viewName(x)) {
            *v = x;
            return true;
        }
    return false;
}

}  // namespace inspect
