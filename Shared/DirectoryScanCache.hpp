#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "Shared/Utf8Path.hpp"

namespace shared {

// ── DirectoryScanCache: a media directory's parsed entries, kept ─────────
//
// The pickers are refilled after every operation, and parsing a save
// folder re-reads every `.card.yaml` / `.floppy.yaml` in full (instances
// carry their whole RAM). This keeps the last scan of one directory and
// reuses it while the directory's files -- names, sizes, modification
// times, read without opening them -- are unchanged. Any change, the
// app's own saves as well as a file copied in from outside, is noticed on
// the next get().
template <class Entry>
class DirectoryScanCache {
public:
    // `scan(dir)` parses the directory; it runs only when the stamp moved.
    template <class Scan>
    const std::vector<Entry>& get(const std::string& dir, Scan&& scan) {
        std::vector<FileStamp> stamp = stampOf(dir);
        if (!m_valid || dir != m_dir || stamp != m_stamp) {
            m_entries = std::forward<Scan>(scan)(dir);
            m_dir = dir;
            m_stamp = std::move(stamp);
            m_valid = true;
        }
        return m_entries;
    }

private:
    struct FileStamp {
        std::string name;
        std::uintmax_t size = 0;
        std::int64_t mtime = 0;
        bool operator==(const FileStamp&) const = default;
    };

    static std::vector<FileStamp> stampOf(const std::string& dir) {
        std::vector<FileStamp> out;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(toPath(dir), ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fe;
            if (!it->is_regular_file(fe)) continue;
            FileStamp s;
            s.name = fromPath(it->path().filename());
            s.size = it->file_size(fe);
            s.mtime = static_cast<std::int64_t>(it->last_write_time(fe).time_since_epoch().count());
            out.push_back(std::move(s));
        }
        // Directory order isn't stable across platforms.
        std::sort(out.begin(), out.end(), [](const FileStamp& a, const FileStamp& b) { return a.name < b.name; });
        return out;
    }

    bool m_valid = false;
    std::string m_dir;
    std::vector<FileStamp> m_stamp;
    std::vector<Entry> m_entries;
};

} // namespace shared
