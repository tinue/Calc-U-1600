#pragma once
#include <optional>
#include <string>
#include <vector>

#include "../Connector/ExpansionCard.hpp"
#include "../Connector/MediaSave.hpp"
#include "PresetFile.hpp"

// ── Preset `saveas:` for the headless tools ───────────────────────────────
//
// Writes a machine's slot card or floppy for a `saveas:` step the way the
// GUI does, by the shared rules of Core/Connector/MediaSave.hpp. Model-
// neutral: the caller fills PresetMedia from its machine and the loader's
// PresetLoadResult.

// What a machine holds for `saveas:` -- either model, filled by the caller
// from the machine and the loader's PresetLoadResult.
struct PresetMediaSlot {
    const ExpansionCard* card = nullptr;  // null = empty slot
    std::string sourcePath;               // PresetLoadResult::slotNResolvedPath
    std::string romFile;                  // PresetLoadResult::slotNRomFile: never saved
};
struct PresetMedia {
    PresetMediaSlot slots[2];
    std::optional<std::vector<uint8_t>> floppyImage;  // none = no disk in the drive
};

// A preset's `saveas:` for callers without the GUI (the headless tools,
// tests): checks, formats and writes.
inline bool savePresetMedia(const PresetSaveAsRequest& request, const MediaSaveFolders& folders,
                            const PresetMedia& media, std::string* error) {
    const bool floppy = request.target == PresetStep::SaveAsTarget::Floppy;
    std::string path;
    const MediaSaveRefusal refusal = planMediaSave(floppy ? MediaKind::Floppy : MediaKind::Card, request.name,
                                                   request.path, request.isTemplate, folders, &path);
    if (refusal != MediaSaveRefusal::None) {
        *error = mediaSaveRefusalText(refusal, request.name, path);
        return false;
    }

    std::string text;
    if (floppy) {
        if (!media.floppyImage) {
            *error = "there's no disk in the drive";
            return false;
        }
        text = formatFloppyFile(request.name, *media.floppyImage, request.isTemplate);
    } else {
        const int n = request.target == PresetStep::SaveAsTarget::Slot1 ? 1 : 2;
        const PresetMediaSlot& slot = media.slots[n - 1];
        if (!slot.card || slot.sourcePath.empty()) {
            *error = "slot " + std::to_string(n) + " has no card file to save from";
            return false;
        }
        if (!slot.romFile.empty()) {
            *error = "slot " + std::to_string(n) + ": the ROM comes from " + slot.romFile +
                     ", not from the module file, so it can't be saved";
            return false;
        }
        if (!formatCardSave(*slot.card, slot.sourcePath, request.name, request.isTemplate, &text, error))
            return false;
    }
    return writeFileAtomically(path, text, error);
}
