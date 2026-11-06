#include "Shared/TapeBay.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

#include "Shared/AppPaths.hpp"
#include "Shared/MachineSession.hpp"
#include "Shared/Utf8Path.hpp"

namespace shared {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trimmed(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// "<name>.wav" -> "<name>": everything before the last suffix, like
// QFileInfo::completeBaseName().
std::string baseName(const fs::path& p) { return fromPath(p.stem()); }

} // namespace

TapeBay::TapeBay(MachineSession& session, const AppPaths& paths, Events events)
    : m_session(session), m_paths(paths), m_events(std::move(events)) {}

std::vector<std::string> TapeBay::tapeNames() const {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(toPath(m_paths.tapeDir()), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (lower(fromPath(it->path().extension())) != ".wav") continue;
        names.push_back(baseName(it->path()));
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    return names;
}

std::string TapeBay::currentName() const {
    const TapeDeck::Status status = m_session.tapeStatus();
    if (status.mode == TapeDeck::Mode::Empty) return {};
    return baseName(toPath(status.path));
}

bool TapeBay::exists(const std::string& name) const {
    std::error_code ec;
    return fs::exists(toPath(m_paths.tapePathFor(name)), ec);
}

bool TapeBay::selectForPlay(const std::string& nameOrEmpty, std::string* error) {
    m_session.tapeEject(error);
    if (nameOrEmpty.empty()) return true;
    return m_session.tapePlay(m_paths.tapePathFor(nameOrEmpty), error);
}

bool TapeBay::recordNew(const std::string& name) {
    const std::string t = trimmed(name);
    if (t.empty()) return false;
    m_session.tapeRecord(m_paths.tapePathFor(t)); // ejects what was in
    return true;
}

void TapeBay::onFrameTick(const TapeDeck::Status& status) {
    const bool recordingMotor = status.mode == TapeDeck::Mode::Record && status.motor;
    if (m_recordingMotor && !recordingMotor && m_events.tapesChanged) m_events.tapesChanged(); // saved at the motor stop
    m_recordingMotor = recordingMotor;
    const std::string error = m_session.tapeTakeLastError();
    if (!error.empty() && m_events.error) m_events.error(error);
}

} // namespace shared
