#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Shared/AppSession.hpp"

namespace shared {

// ── MediaLibrary: the user's own media files, as items ───────────────────
//
// What the save folder holds -- saved cards (instances and the user's own
// templates), disks and tapes -- for a frontend that has no file manager
// to reach them with (a browser keeps them in its own storage): list,
// export a copy, import one, delete one. Bundled cards and disks are not
// listed; they can't be changed.
enum class MediaType { Card, Disk, Tape };

struct MediaItem {
    MediaType type = MediaType::Card;
    std::string name;      // module / disk / tape name, as the pickers show it
    std::string description;  // a card's / disk's `description:`; "" = none
    std::string fileName;  // the file's own name ("CE-161 work.card.yaml"), for a download
    std::string path;
    bool isTemplate = false;
    bool inUse = false;    // in a slot, the drive or the recorder: can't be deleted
};

class MediaLibrary {
public:
    explicit MediaLibrary(AppSession& app) : m_app(app) {}

    // Cards, then disks, then tapes; each by name.
    std::vector<MediaItem> items() const;

    // The bytes of the listed item at `path`, for a download; false if
    // there is no such item or it can't be read.
    bool exportBytes(const std::string& path, std::vector<std::uint8_t>* out) const;

    // A `.card.yaml`, `.floppy.yaml` or `.wav` file into the save folder,
    // under the name it carries (a card's module name, a disk's name, a
    // tape's file name). Refused: an unknown kind, a file that doesn't
    // parse, a name a bundled card/disk or a saved item already has.
    OpResult importFile(const std::string& fileName, const std::vector<std::uint8_t>& bytes);

    // Deletes the listed item at `path`. Refused while it is in use.
    OpResult remove(const std::string& path);

private:
    std::optional<MediaItem> find(const std::string& path) const;

    AppSession& m_app;
};

} // namespace shared
