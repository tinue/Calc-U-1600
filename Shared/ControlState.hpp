#pragma once
#include <string>

#include "Shared/MachineModel.hpp"

namespace shared {

class MachineSession;

// The 60-pin peripherals the control bar toggles.
enum class Peripheral { CE150, CE1600P, CE158 };

// One peripheral as the frontend shows it.
struct PeripheralState {
    bool offered = false;       // listed in Machine ▸ Peripherals
    bool onControlBar = false;  // a toggle on the control bar (the model's own peripherals)
    bool attached = false;
    bool enabled = false;       // may be toggled now: no attached peripheral rules it out
};

// Which plotter paper the paper panel shows.
enum class PaperKind { None, CE150, CE1600P };

// ── ControlState: what the pickers, menus and panels offer right now ─────
//
// Every rule about which control a model has and what excludes what, so a
// frontend only renders it. Pulled after every operation (the busy state
// going false) along with the rest of the UI.
struct ControlState {
    Model model = Model::PC1500A;
    int slotCount = 0;  // memory-module slots: 1 on a PC-1500(A), 2 on a PC-1600

    PeripheralState ce150;
    PeripheralState ce1600p;
    PeripheralState ce158;

    // The CE-1600F disk picker: shown on every PC-1600 (so the bar doesn't
    // jump), usable only while the CE-1600P (its box) is attached.
    bool floppyShown = false;
    bool floppyEnabled = false;
    // The tape picker: while a tape interface (CE-150, CE-1600P) is attached.
    bool tapeShown = false;

    // Machine ▸ ROM pickers. The PC-1500A is A04-only, so only the plain
    // PC-1500 gets one.
    bool pc1500RomMenu = false;
    bool pc1600RomMenu = false;
    bool ce1600pRomMenu = false;

    // File ▸ Mount Directory (S3:): PC-1600 only, and only on a frontend
    // that has host directories.
    bool hostDriveOffered = false;
    std::string hostDriveDir;  // the mounted directory, empty = none

    // The docked panels.
    PaperKind paper = PaperKind::None;
    bool printerShown = false;  // CE-158 printer output

    const PeripheralState& peripheral(Peripheral p) const;
};

// The control state of `session`'s live machine. `hostDirectories`: the
// frontend can mount a host directory (a browser can't).
ControlState controlStateOf(const MachineSession& session, bool hostDirectories);

} // namespace shared
