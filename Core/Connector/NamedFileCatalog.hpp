#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

// ── Directory catalogues of named files ────────────────────────────────
//
// The shared mechanism behind memory cards (`*.card.yaml`, keyed by
// `module-name` -- MemoryCardCatalog.hpp) and floppy disks (`*.floppy.yaml`,
// keyed by `disk-name` -- FloppyImageFile.hpp): scan a directory for files
// with one suffix, index them by the name each declares, and resolve a name
// against an ordered directory list (bundled first, then the save folder).
// Each format only supplies its suffix and a function reading the name.

namespace named_file_detail {

inline bool hasSuffix(const std::string& fileName, const std::string& suffix) {
    return fileName.size() > suffix.size() &&
           fileName.compare(fileName.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool readTextFile(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

}  // namespace named_file_detail

// Parses every "*<suffix>" file in `dir` (non-recursive) with
// `parse(text, path, Entry* out, std::string* err) -> bool`. A file that
// fails to parse is skipped, with "<filename>: <error>" appended to `*error`
// (newline-separated) -- one bad file never hides the rest. A missing /
// unreadable directory yields an empty list and sets `*error`. Entries come
// back sorted by `nameOf(entry)`. `kind` names the file type in errors.
template <typename Entry, typename ParseFn, typename NameOf>
std::vector<Entry> scanNamedFiles(const std::string& dir, const std::string& suffix, const std::string& kind,
                                  ParseFn parse, NameOf nameOf, std::string* error) {
    std::vector<Entry> out;
    auto appendErr = [&](const std::string& msg) {
        if (!error) return;
        if (!error->empty()) *error += "\n";
        *error += msg;
    };

    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    const std::filesystem::directory_iterator end;
    if (ec) {
        if (error) *error = "cannot read " + kind + " directory '" + dir + "': " + ec.message();
        return out;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            appendErr("directory walk stopped: " + ec.message());
            break;
        }
        const std::filesystem::path& p = it->path();
        // Double extensions (".card.yaml") -- path::extension() only sees
        // ".yaml", so match the filename tail directly.
        const std::string name = p.filename().string();
        if (!named_file_detail::hasSuffix(name, suffix)) continue;

        std::string text, parseErr;
        if (!named_file_detail::readTextFile(p.string(), &text)) {
            appendErr(name + ": cannot open");
            continue;
        }
        Entry entry;
        if (!parse(text, p.string(), &entry, &parseErr)) {
            appendErr(name + ": " + parseErr);
            continue;
        }
        out.push_back(std::move(entry));
    }
    std::sort(out.begin(), out.end(), [&](const Entry& a, const Entry& b) { return nameOf(a) < nameOf(b); });
    return out;
}

// Resolves `name` against `dirs` in order, via `scan(dir, std::string* err)`:
// the first directory holding exactly one entry with that name wins; two in
// the SAME directory is an error (an ambiguous catalogue is a setup
// mistake, not a pick-one case). Missing directories and empty strings are
// skipped; parse problems met on the way are reported with a not-found.
template <typename ScanFn, typename NameOf>
bool resolveNamedFile(const std::vector<std::string>& dirs, const std::string& name, const std::string& kind,
                      ScanFn scan, NameOf nameOf, std::string* outPath, std::string* error) {
    std::vector<std::string> searched;
    std::string scanNotes;
    for (const auto& dir : dirs) {
        if (dir.empty()) continue;
        searched.push_back(dir);

        std::string scanErr;
        const auto entries = scan(dir, &scanErr);
        decltype(entries.data()) hit = nullptr;
        for (const auto& e : entries) {
            if (nameOf(e) != name) continue;
            if (hit) {
                if (error)
                    *error = kind + " name '" + name + "' is declared by more than one file in '" + dir + "' (" +
                             hit->filePath + ", " + e.filePath + ")";
                return false;
            }
            hit = &e;
        }
        if (hit) {
            if (outPath) *outPath = hit->filePath;
            return true;
        }
        if (!scanErr.empty()) {
            if (!scanNotes.empty()) scanNotes += "; ";
            scanNotes += scanErr;
        }
    }
    if (error) {
        std::string where;
        for (const auto& d : searched) {
            if (!where.empty()) where += ", ";
            where += "'" + d + "'";
        }
        *error = "no " + kind + " named '" + name + "' in " + (where.empty() ? "any " + kind + " directory" : where);
        if (!scanNotes.empty()) *error += " (some files failed to parse: " + scanNotes + ")";
    }
    return false;
}
