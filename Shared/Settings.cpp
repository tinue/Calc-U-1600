#include "Shared/Settings.hpp"

#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "Shared/Utf8Path.hpp"

namespace shared {

namespace fs = std::filesystem;

namespace {
std::string absolutePath(const std::string& path) {
    std::error_code ec;
    fs::path abs = fs::absolute(toPath(path), ec);
    if (ec) return path;
    abs = abs.lexically_normal();
    // No trailing separator ("/a/b/" -> "/a/b"), like QDir::absolutePath().
    if (!abs.has_filename() && abs.has_relative_path()) abs = abs.parent_path();
    return fromPath(abs);
}
} // namespace

std::string Settings::group(OpenFolder folder) {
    switch (folder) {
        case OpenFolder::Samples: return "preset/";
        case OpenFolder::Basic: return "basic/";
        case OpenFolder::Assembly: return "assembly/";
        case OpenFolder::HostDrive: return "hostDrive/";
    }
    return "preset/";
}

std::string Settings::str(const std::string& key, const std::string& fallback) const {
    const auto v = m_store.get(key);
    return v ? *v : fallback;
}

int Settings::integer(const std::string& key, int fallback) const {
    const auto v = m_store.get(key);
    if (!v || v->empty()) return fallback;
    char* end = nullptr;
    const long n = std::strtol(v->c_str(), &end, 10);
    return (end && *end == '\0') ? static_cast<int>(n) : 0;
}

bool Settings::boolean(const std::string& key, bool fallback) const {
    const auto v = m_store.get(key);
    if (!v || v->empty()) return fallback;
    return *v == "true" || *v == "1";
}

void Settings::setOrRemove(const std::string& key, const std::string& value) {
    if (value.empty())
        m_store.remove(key);
    else
        m_store.set(key, value);
}

void Settings::rememberOpenDir(OpenFolder folder, const std::string& dir) {
    m_store.set(group(folder) + "lastOpenDir", absolutePath(dir));
}

void Settings::rememberOpenFile(OpenFolder folder, const std::string& filePath) {
    rememberOpenDir(folder, fromPath(toPath(absolutePath(filePath)).parent_path()));
}

std::string Settings::openStartDir(OpenFolder folder, const std::string& homeDir) const {
    std::string dir = openDir(folder);
    if (dir.empty()) dir = lastOpenDir(folder);
    return dir.empty() ? homeDir : dir;
}

int Settings::dapPort() const {
    const int port = integer("debug/dapPort", kDefaultDapPort);
    return port >= 1024 && port <= 65535 ? port : kDefaultDapPort;
}

Model Settings::startupModel() const {
    const std::string pref = startupModelPreference();
    for (Model m : {Model::PC1500, Model::PC1500A, Model::PC1600}) {
        if (pref == modelKey(m)) return m;
    }
    const int last = lastUsedModel();
    if (last >= static_cast<int>(Model::PC1500) && last <= static_cast<int>(Model::PC1600)) {
        return static_cast<Model>(last);
    }
    return Model::PC1500A;
}

} // namespace shared
