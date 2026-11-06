#pragma once

// The emulated models and ROM versions every frontend picks between.
namespace shared {

enum class Model { PC1500, PC1500A, PC1600 };
// PC-1600 calculator ROM version: New = PEEK #(0,&7FFF) 4/5, Old = 130.
enum class PC1600RomVersion { New, Old };
// CE-1600P ROM version: New = PEEK #(5,&7FFE) 5, Old = 4. Independent of the
// PC-1600 calculator ROM version; the CE-1600F in the same box follows it.
enum class CE1600PRomVersion { New, Old };

// The per-model settings key ("PC1500"/"PC1500A"/"PC1600") used by the
// startup-model preference and the per-model default preset; lowercased,
// it's also that model's preset file extension.
constexpr const char* modelKey(Model model) {
    switch (model) {
        case Model::PC1500: return "PC1500";
        case Model::PC1500A: return "PC1500A";
        case Model::PC1600: return "PC1600";
    }
    return "";
}

} // namespace shared
