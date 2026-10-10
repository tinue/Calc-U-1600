#pragma once
#include "../Preset/PresetMedia.hpp"
#include "../Preset/PresetRunner.hpp"
#include "PC1600Machine.hpp"

// What a PC-1600 holds for a preset's `saveas:` `request` (savePresetMedia()):
// both slots, as the loader attached them (`armed`, from onArmed), and --
// for a floppy save only -- the disk in the CE-1600F.
inline PresetMedia pc1600PresetMedia(const PC1600Machine& machine, const PresetLoadResult& armed,
                                     const PresetSaveAsRequest& request) {
    PresetMedia media;
    media.slots[0] = {machine.memory().slotCard(1), armed.slot1ResolvedPath, armed.slot1RomFile};
    media.slots[1] = {machine.memory().slotCard(2), armed.slot2ResolvedPath, armed.slot2RomFile};
    if (request.target == PresetStep::SaveAsTarget::Floppy && machine.ce1600fAttached() && machine.ce1600fHasDisk())
        media.floppyImage = machine.ce1600fDiskImage();
    return media;
}
