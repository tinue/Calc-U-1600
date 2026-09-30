#pragma once
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../Connector/BatteryCardInstance.hpp"
#include "../Connector/FloppyImageFile.hpp"
#include "../Connector/MemoryCardCatalog.hpp"
#include "../Preset/PresetFile.hpp"
#include "PC1600Machine.hpp"

// ── Preset `saveas:` for the headless tools ───────────────────────────────
//
// Writes the live slot card or floppy of a PC1600Machine the way the GUI's
// MemoryModuleManager / FloppyDiskManager do, for callers without the GUI
// (pc1600_cli, tests): a card is spliced into the definition it was loaded
// from (`slotSourcePath`, the loader's slot<N>ResolvedPath -- a card file
// keeps its layout and comments), a floppy is formatted whole. A request
// with a `path` writes that file; a by-name one goes to
// `<saveDir>/<name>.card.yaml` / `.floppy.yaml` (namedFileName(), as in
// AppPaths).
namespace pc1600_preset_media {

inline bool writeAtomically(const std::filesystem::path& path, const std::string& text, std::string* error) {
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!(out << text)) {
            *error = "cannot write " + tmp.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        *error = "cannot write " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace pc1600_preset_media

inline bool savePC1600PresetMedia(PC1600Machine& machine, const PresetSaveAsRequest& request,
                                  const std::string& saveDir, const std::string& slot1SourcePath,
                                  const std::string& slot2SourcePath, std::string* error) {
    using namespace pc1600_preset_media;
    const bool floppy = request.target == PresetStep::SaveAsTarget::Floppy;
    std::filesystem::path path = request.path;
    if (path.empty()) {
        if (saveDir.empty()) {
            *error = "no save directory for '" + request.name + "' (a by-name saveas needs one)";
            return false;
        }
        path = std::filesystem::path(saveDir) / namedFileName(request.name, floppy ? kFloppyFileSuffix : kCardFileSuffix);
    }

    if (floppy) {
        if (!machine.ce1600fAttached() || !machine.ce1600fHasDisk()) {
            *error = "there's no disk in the drive";
            return false;
        }
        return writeAtomically(path, formatFloppyFile(request.name, machine.ce1600fDiskImage(), request.isTemplate),
                               error);
    }

    const bool slot1 = request.target == PresetStep::SaveAsTarget::Slot1;
    const std::string& source = slot1 ? slot1SourcePath : slot2SourcePath;
    if (source.empty()) {
        *error = std::string("slot ") + (slot1 ? "1" : "2") + " has no card file to save from";
        return false;
    }
    std::string sourceText;
    if (!named_file_detail::readTextFile(source, &sourceText)) {
        *error = "cannot read " + source;
        return false;
    }
    const std::vector<uint8_t> image = slot1 ? machine.memory().slot1CardImage() : machine.memory().slot2CardImage();
    const int bankCount = slot1 ? machine.memory().slot1CardBankCount() : machine.memory().slot2CardBankCount();
    if (image.empty()) {
        *error = "no card contents to save";
        return false;
    }
    std::string sourceName;
    MemoryCardCatalogEntry entry;
    if (readMemoryCardCatalogEntry(source, &entry, nullptr)) sourceName = entry.moduleName;
    std::string spliced;
    if (!spliceBatteryCardInstance(sourceText, request.name, sourceName,
                                   formatBatteryCardInitialContentBlock(bankCount, image), &spliced, error,
                                   currentIso8601Timestamp(), request.isTemplate))
        return false;
    return writeAtomically(path, spliced, error);
}
