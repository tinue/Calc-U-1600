#include "Shared/ControlState.hpp"

#include "Shared/MachineSession.hpp"

namespace shared {

ControlState controlStateOf(const MachineSession& session, bool hostDirectories) {
    ControlState s;
    s.model = session.currentModel();
    const bool isPC1600 = s.model == Model::PC1600;
    s.slotCount = session.slotCount();

    // The CE-1600P excludes both the CE-150 and the CE-158 -- the CE-158
    // with the CE-1600P isn't supported yet (TODO.md, IO7N). Each model's
    // own peripherals get a control-bar button; Machine ▸ Peripherals has
    // all of them (the CE-1600P on a PC-1600 only).
    s.ce150.attached = session.ce150Attached();
    s.ce1600p.attached = session.ce1600pAttached();
    s.ce158.attached = session.ce158Attached();

    s.ce150.offered = true;
    s.ce150.onControlBar = !isPC1600;
    s.ce150.enabled = !s.ce1600p.attached;

    s.ce158.offered = true;
    s.ce158.onControlBar = !isPC1600;
    s.ce158.enabled = !s.ce1600p.attached;

    s.ce1600p.offered = isPC1600;
    s.ce1600p.onControlBar = isPC1600;
    s.ce1600p.enabled = isPC1600 && !s.ce150.attached && !s.ce158.attached;

    // The CE-1600F comes in the CE-1600P's box.
    s.floppyShown = isPC1600;
    s.floppyEnabled = s.ce1600p.attached;
    s.tapeShown = session.tapeInterfaceAttached();

    s.pc1500RomMenu = s.model == Model::PC1500;
    s.pc1600RomMenu = isPC1600;
    s.ce1600pRomMenu = isPC1600;

    s.hostDriveOffered = isPC1600 && hostDirectories;
    s.hostDriveDir = session.hostDriveDirectory();

    s.paper = s.ce150.attached ? PaperKind::CE150 : s.ce1600p.attached ? PaperKind::CE1600P : PaperKind::None;
    s.printerShown = s.ce158.attached;
    return s;
}

} // namespace shared
