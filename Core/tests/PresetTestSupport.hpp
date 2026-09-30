#pragma once
#include <filesystem>
#include <fstream>
#include <string>

#include "../Preset/PresetFile.hpp"

// Shared by ce155_tests.cpp/ce1638plus_tests.cpp: parsePresetFile() only
// takes a path -- write `yaml` to a scratch file first, matching this
// project's own test convention of exercising real file I/O for this
// parser (no in-memory-string overload exists). `scratchPath` is caller-
// supplied so concurrently-run test binaries don't share one file.
//
// The parser reads and classifies a `program: file:` (Core/ProgramFile), so
// the stand-in files most tests name exist next to the scratch preset:
// `x.bin` (headerless code) and `x.bas` (a BASIC listing). A test that
// needs other bytes writes its own file.
inline bool parsePresetString(const std::string& yaml, const std::string& scratchPath, PresetFile* out,
                               std::string* error) {
    const std::filesystem::path dir = std::filesystem::path(scratchPath).parent_path();
    std::filesystem::create_directories(dir);
    if (!std::filesystem::exists(dir / "x.bin")) {
        std::ofstream bin(dir / "x.bin", std::ios::binary);
        const unsigned char code[] = {0xFD, 0xA8, 0x9A, 0x00, 0x01, 0x02, 0x03, 0x04};
        bin.write(reinterpret_cast<const char*>(code), sizeof code);
    }
    if (!std::filesystem::exists(dir / "x.bas")) std::ofstream(dir / "x.bas") << "10 PRINT 1\n";
    std::ofstream f(scratchPath);
    f << yaml;
    f.close();
    return parsePresetFile(scratchPath, out, error);
}
