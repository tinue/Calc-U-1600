#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "BatteryCardInstance.hpp"
#include "ExpansionCard.hpp"
#include "FloppyImageFile.hpp"
#include "MemoryCardCatalog.hpp"
#include "NamedFileCatalog.hpp"

// ── Saving a card or a floppy under a name ───────────────────────────────
//
// The one set of rules for every save of a memory card or a floppy under a
// new name: the GUI's Name & Save and preset `saveas:` (MemoryModuleManager,
// FloppyDiskManager) and the headless tools' `saveas:` (savePresetMedia(),
// Core/Preset/PresetMedia.hpp).
// The GUI adds only what is interactive (an already saved card, a name
// collision), its own wording, and retargeting the autosave.
//
//   * planMediaSave() -- where the file goes, and whether it may: a
//     by-name save goes to `<saves>/<name><suffix>` (namedFileName()); a
//     preset's `file:` form writes exactly that file, with no name checks.
//   * formatCardSave() / formatFloppyFile() -- the text: a card is spliced
//     into the file it was loaded from (so it keeps its layout, comments and
//     `format-version`), a floppy is written whole.

struct MediaSaveFolders {
    std::string bundled;  // the bundled cards and disks (read-only)
    std::string saves;    // where by-name saves go; empty = none
};

enum class MediaKind { Card, Floppy };

// Why a save is refused. The GUI words these itself (tr()); the headless
// tools use mediaSaveRefusalText().
enum class MediaSaveRefusal {
    None,
    EmptyName,
    QuoteInName,     // a name is written double-quoted into the file
    NoSaveFolder,    // a by-name save without a save folder
    TemplateName,    // a bundled name, or a user template's (unless saving a template)
    TemplateFile,    // the target file is a template, never overwritten (unless saving a template)
};

namespace media_save_detail {

inline bool nameIn(MediaKind kind, const std::string& dir, const std::string& name, bool templatesOnly) {
    if (dir.empty()) return false;
    if (kind == MediaKind::Card) {
        for (const auto& e : scanMemoryCardDirectory(dir, nullptr))
            if (e.moduleName == name && (!templatesOnly || e.isTemplate)) return true;
    } else {
        for (const auto& e : scanFloppyDirectory(dir, nullptr))
            if (e.diskName == name && (!templatesOnly || e.isTemplate)) return true;
    }
    return false;
}

inline bool isTemplateFile(MediaKind kind, const std::string& path) {
    if (kind == MediaKind::Card) {
        MemoryCardCatalogEntry e;
        return readMemoryCardCatalogEntry(path, &e, nullptr) && e.isTemplate;
    }
    FloppyCatalogEntry e;
    return readFloppyCatalogEntry(path, &e, nullptr) && e.isTemplate;
}

}  // namespace media_save_detail

// Where a save of `name` goes (`*path`), or why it may not. `name` is
// already trimmed. `explicitPath` = a preset's `file:` form.
inline MediaSaveRefusal planMediaSave(MediaKind kind, const std::string& name, const std::string& explicitPath,
                                      bool asTemplate, const MediaSaveFolders& folders, std::string* path) {
    using namespace media_save_detail;
    if (name.empty()) return MediaSaveRefusal::EmptyName;
    if (name.find('"') != std::string::npos) return MediaSaveRefusal::QuoteInName;
    if (!explicitPath.empty()) {
        *path = explicitPath;
        return MediaSaveRefusal::None;
    }
    if (folders.saves.empty()) return MediaSaveRefusal::NoSaveFolder;
    // A bundled name would shadow the saved file on lookup. One of the
    // user's own templates may only be replaced by another template save --
    // a preset re-making its template.
    if (nameIn(kind, folders.bundled, name, false) || (!asTemplate && nameIn(kind, folders.saves, name, true)))
        return MediaSaveRefusal::TemplateName;
    *path = (std::filesystem::path(folders.saves) /
             namedFileName(name, kind == MediaKind::Card ? kCardFileSuffix : kFloppyFileSuffix))
                .string();
    if (!asTemplate && isTemplateFile(kind, *path)) return MediaSaveRefusal::TemplateFile;
    return MediaSaveRefusal::None;
}

inline std::string mediaSaveRefusalText(MediaSaveRefusal refusal, const std::string& name, const std::string& path) {
    switch (refusal) {
        case MediaSaveRefusal::None: return {};
        case MediaSaveRefusal::EmptyName: return "the name is empty";
        case MediaSaveRefusal::QuoteInName: return "'" + name + "': a name cannot contain '\"'";
        case MediaSaveRefusal::NoSaveFolder: return "no save directory for '" + name + "' (a by-name saveas needs one)";
        case MediaSaveRefusal::TemplateName: return "'" + name + "' is a template's name";
        case MediaSaveRefusal::TemplateFile: return path + " is a template file and is never overwritten";
    }
    return {};
}

// The text of `card` saved as `name`: its live contents spliced into
// `sourcePath`, the file it was loaded from.
inline bool formatCardSave(const ExpansionCard& card, const std::string& sourcePath, const std::string& name,
                           bool asTemplate, std::string* text, std::string* error) {
    std::string sourceText;
    if (!named_file_detail::readTextFile(sourcePath, &sourceText)) {
        *error = "cannot read " + sourcePath;
        return false;
    }
    const std::vector<uint8_t> image = card.debugImage();
    if (image.empty()) {
        *error = "no card contents to save";
        return false;
    }
    // The bank count passes through unchanged: -1 (no bank concept) and 1
    // format differently (formatBatteryCardInitialContentBlock()).
    return spliceBatteryCardInstance(sourceText, name, card.moduleName(),
                                     formatBatteryCardInitialContentBlock(card.debugBankCount(), image), text, error,
                                     currentIso8601Timestamp(), asTemplate);
}

// Writes `text` to `path` through a temporary file next to it, so a failed
// write never leaves half a file.
inline bool writeFileAtomically(const std::string& path, const std::string& text, std::string* error) {
    const std::filesystem::path tmp = path + ".tmp";
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
        *error = "cannot write " + path + ": " + ec.message();
        return false;
    }
    return true;
}
