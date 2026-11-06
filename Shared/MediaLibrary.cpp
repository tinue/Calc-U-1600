#include "Shared/MediaLibrary.hpp"

#include <algorithm>
#include <filesystem>
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

bool endsWith(const std::string& s, const std::string& tail) {
    if (s.size() < tail.size()) return false;
    return std::equal(tail.rbegin(), tail.rend(), s.rbegin(),
                      [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) ==
                                                  std::tolower(static_cast<unsigned char>(b)); });
}

std::string fileNameOf(const std::string& path) { return fromPath(toPath(path).filename()); }

bool sameName(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

} // namespace

bool MediaLibrary::importable(const std::string& fileName) {
    return endsWith(fileName, kCardFileSuffix) || endsWith(fileName, kFloppyFileSuffix) || endsWith(fileName, ".wav");
}

std::vector<MediaItem> MediaLibrary::items() const {
    std::vector<MediaItem> out;
    const std::string saves = m_app.paths().instanceDir();
    const ModuleBay& modules = m_app.modules();
    const int slots = m_app.machine().slotCount();

    for (const auto& e : scanMemoryCardDirectory(saves, nullptr)) {
        MediaItem item{MediaType::Card, e.moduleName, fileNameOf(e.filePath), e.filePath, e.isTemplate, false};
        for (int slot = 1; slot <= slots; ++slot)
            if (modules.selectedModuleName(slot) == e.moduleName) item.inUse = true;
        out.push_back(std::move(item));
    }
    const bool driveOn = m_app.machine().ce1600pAttached();
    for (const auto& e : scanFloppyDirectory(saves, nullptr)) {
        MediaItem item{MediaType::Disk, e.diskName, fileNameOf(e.filePath), e.filePath, e.isTemplate, false};
        item.inUse = driveOn && m_app.floppy().selectedDiskName() == e.diskName;
        out.push_back(std::move(item));
    }
    const std::string inDeck = m_app.tapes().currentName();
    for (const std::string& name : m_app.tapes().tapeNames()) {
        const std::string path = m_app.paths().tapePathFor(name);
        out.push_back(MediaItem{MediaType::Tape, name, fileNameOf(path), path, false, name == inDeck});
    }
    return out;
}

bool MediaLibrary::exportBytes(const MediaItem& item, std::vector<std::uint8_t>* out) const {
    return readWholeFile(toPath(item.path), out);
}

OpResult MediaLibrary::importFile(const std::string& fileName, const std::vector<std::uint8_t>& bytes) {
    const std::string text(bytes.begin(), bytes.end());
    const std::string bundled = m_app.paths().bundledResourcesDir();
    const std::string saves = m_app.paths().instanceDir();

    if (endsWith(fileName, kCardFileSuffix)) {
        MemoryCardCatalogEntry entry;
        std::string err;
        if (!memory_card_catalog_detail::parseEntry(text, fileName, &entry, &err))
            return OpResult::fail(inQuotes(fileName) + " isn't a card file: " + err);
        for (const auto& dir : {bundled, saves})
            for (const auto& e : scanMemoryCardDirectory(dir, nullptr))
                if (e.moduleName == entry.moduleName)
                    return OpResult::fail("A card named " + inQuotes(entry.moduleName) + " already exists.");
        const std::string path = m_app.paths().instancePathFor(entry.moduleName);
        if (!atomicWriteFile(path, text)) return OpResult::fail("Couldn't write " + inQuotes(path) + ".");
        return {};
    }

    if (endsWith(fileName, kFloppyFileSuffix)) {
        FloppyFile disk;
        std::string err;
        if (!parseFloppyFile(text, &disk, &err)) return OpResult::fail(inQuotes(fileName) + " isn't a disk file: " + err);
        for (const auto& dir : {bundled, saves})
            for (const auto& e : scanFloppyDirectory(dir, nullptr))
                if (e.diskName == disk.diskName)
                    return OpResult::fail("A disk named " + inQuotes(disk.diskName) + " already exists.");
        const std::string path = m_app.paths().floppyInstancePathFor(disk.diskName);
        if (!atomicWriteFile(path, text)) return OpResult::fail("Couldn't write " + inQuotes(path) + ".");
        return {};
    }

    if (endsWith(fileName, ".wav")) {
        const std::string name = trimmed(fileName.substr(0, fileName.size() - 4));
        if (name.empty() || name.find_first_of("/\\:") != std::string::npos)
            return OpResult::fail(inQuotes(fileName) + " isn't a usable tape name.");
        for (const std::string& existing : m_app.tapes().tapeNames())
            if (sameName(existing, name)) return OpResult::fail("A tape named " + inQuotes(name) + " already exists.");
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

OpResult MediaLibrary::remove(const MediaItem& item) {
    for (const MediaItem& current : items()) {
        if (current.path != item.path) continue;
        if (current.inUse)
            return OpResult::fail(inQuotes(item.name) + " is in use. Take it out first.");
        std::error_code ec;
        if (!fs::remove(toPath(item.path), ec) || ec) return OpResult::fail("Couldn't delete " + inQuotes(item.name) + ".");
        return {};
    }
    return OpResult::fail(inQuotes(item.name) + " is no longer there.");
}

} // namespace shared
