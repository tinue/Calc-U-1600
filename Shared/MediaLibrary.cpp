#include "Shared/MediaLibrary.hpp"

#include <filesystem>
#include <optional>
#include <system_error>

#include "Audio/WavFile.hpp"
#include "Connector/FloppyImageFile.hpp"
#include "Connector/MemoryCardCatalog.hpp"
#include "FileIO.hpp"
#include "Shared/Text.hpp"
#include "Shared/Utf8Path.hpp"

namespace shared {

namespace fs = std::filesystem;

namespace {

std::string fileNameOf(const std::string& path) { return fromPath(toPath(path).filename()); }

} // namespace

std::vector<MediaItem> MediaLibrary::items() const {
    std::vector<MediaItem> out;
    const ModuleBay& modules = m_app.modules();
    const int slots = m_app.machine().slotCount();

    for (const auto& e : modules.instanceCards()) {
        MediaItem item{MediaType::Card, e.moduleName, e.description, fileNameOf(e.filePath), e.filePath, e.isTemplate, false};
        for (int slot = 1; slot <= slots; ++slot)
            if (modules.selectedModuleName(slot) == e.moduleName) item.inUse = true;
        out.push_back(std::move(item));
    }
    const bool driveOn = m_app.machine().ce1600pAttached();
    for (const auto& e : m_app.floppy().instanceDisks()) {
        MediaItem item{MediaType::Disk, e.diskName, e.description, fileNameOf(e.filePath), e.filePath, e.isTemplate, false};
        item.inUse = driveOn && m_app.floppy().selectedDiskName() == e.diskName;
        out.push_back(std::move(item));
    }
    const std::string inDeck = m_app.tapes().currentName();
    for (const std::string& name : m_app.tapes().tapeNames()) {
        const std::string path = m_app.paths().tapePathFor(name);
        out.push_back(MediaItem{MediaType::Tape, name, {}, fileNameOf(path), path, false, name == inDeck});
    }
    return out;
}

std::optional<MediaItem> MediaLibrary::find(const std::string& path) const {
    for (MediaItem& item : items())
        if (item.path == path) return std::move(item);
    return std::nullopt;
}

bool MediaLibrary::exportBytes(const std::string& path, std::vector<std::uint8_t>* out) const {
    return find(path) && readWholeFile(toPath(path), out);
}

OpResult MediaLibrary::importFile(const std::string& fileName, const std::vector<std::uint8_t>& bytes) {
    const std::string text(bytes.begin(), bytes.end());
    if (endsWithIgnoringCase(fileName, kCardFileSuffix)) {
        MemoryCardCatalogEntry entry;
        std::string err;
        if (!memory_card_catalog_detail::parseEntry(text, fileName, &entry, &err))
            return OpResult::fail(inQuotes(fileName) + " isn't a card file: " + err);
        for (const auto* cards : {&m_app.modules().bundledCards(), &m_app.modules().instanceCards()})
            for (const auto& e : *cards)
                if (e.moduleName == entry.moduleName)
                    return OpResult::fail("A card named " + inQuotes(entry.moduleName) + " already exists.");
        const std::string path = m_app.paths().instancePathFor(entry.moduleName);
        if (!atomicWriteFile(path, text)) return OpResult::fail("Couldn't write " + inQuotes(path) + ".");
        return {};
    }

    if (endsWithIgnoringCase(fileName, kFloppyFileSuffix)) {
        FloppyFile disk;
        std::string err;
        if (!parseFloppyFile(text, &disk, &err)) return OpResult::fail(inQuotes(fileName) + " isn't a disk file: " + err);
        for (const auto* disks : {&m_app.floppy().bundledDisks(), &m_app.floppy().instanceDisks()})
            for (const auto& e : *disks)
                if (e.diskName == disk.diskName)
                    return OpResult::fail("A disk named " + inQuotes(disk.diskName) + " already exists.");
        const std::string path = m_app.paths().floppyInstancePathFor(disk.diskName);
        if (!atomicWriteFile(path, text)) return OpResult::fail("Couldn't write " + inQuotes(path) + ".");
        return {};
    }

    if (endsWithIgnoringCase(fileName, ".wav")) {
        const std::string name = trimmed(fileName.substr(0, fileName.size() - 4));
        if (name.empty() || name.find_first_of("/\\:") != std::string::npos)
            return OpResult::fail(inQuotes(fileName) + " isn't a usable tape name.");
        for (const std::string& existing : m_app.tapes().tapeNames())
            if (sameIgnoringCase(existing, name)) return OpResult::fail("A tape named " + inQuotes(name) + " already exists.");
        const std::string path = m_app.paths().tapePathFor(name);
        if (!atomicWriteFile(path, text)) return OpResult::fail("Couldn't write " + inQuotes(path) + ".");
        WavData wav;
        std::string err;
        if (!readWav(path, wav, err)) {
            std::error_code ec;
            fs::remove(toPath(path), ec);
            return OpResult::fail(inQuotes(fileName) + " isn't a WAV file the tape recorder plays: " + err);
        }
        return {};
    }

    return OpResult::fail(inQuotes(fileName) + " isn't a card (.card.yaml), disk (.floppy.yaml) or tape (.wav).");
}

OpResult MediaLibrary::remove(const std::string& path) {
    const std::optional<MediaItem> item = find(path);
    if (!item) return OpResult::fail("It is no longer there.");
    if (item->inUse) return OpResult::fail(inQuotes(item->name) + " is in use. Take it out first.");
    std::error_code ec;
    if (!fs::remove(toPath(path), ec) || ec) return OpResult::fail("Couldn't delete " + inQuotes(item->name) + ".");
    return {};
}

} // namespace shared
