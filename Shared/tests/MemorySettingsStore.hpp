#pragma once
#include <map>

#include "Shared/Settings.hpp"

// In-memory shared::SettingsStore for tests.
class MemorySettingsStore : public shared::SettingsStore {
public:
    std::map<std::string, std::string> values;
    std::optional<std::string> get(const std::string& key) const override {
        const auto it = values.find(key);
        if (it == values.end()) return std::nullopt;
        return it->second;
    }
    void set(const std::string& key, const std::string& value) override { values[key] = value; }
    void remove(const std::string& key) override { values.erase(key); }
};
