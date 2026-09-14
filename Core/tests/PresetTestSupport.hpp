#pragma once
#include <fstream>
#include <string>

#include "../PC1500/PresetFile.hpp"

// Shared by ce155_tests.cpp/ce1638plus_tests.cpp: parsePresetFile() only
// takes a path -- write `yaml` to a scratch file first, matching this
// project's own test convention of exercising real file I/O for this
// parser (no in-memory-string overload exists). `scratchPath` is caller-
// supplied so concurrently-run test binaries don't share one file.
inline bool parsePresetString(const std::string& yaml, const std::string& scratchPath, PresetFile* out,
                               std::string* error) {
    std::ofstream f(scratchPath);
    f << yaml;
    f.close();
    return parsePresetFile(scratchPath, out, error);
}
