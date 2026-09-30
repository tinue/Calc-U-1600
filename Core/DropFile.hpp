#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ── What a dropped file is ─────────────────────────────────────────────
//
// A file dropped onto the window (or, on macOS, onto the Dock icon) goes to
// the loader its content names -- never its extension:
//   - a preset: text with a top-level `model:` key, which every preset needs
//     (parsePresetFile(): "'model' is required"). Not left to libsharpdx: a
//     preset with a character outside the Sharp set isn't `text` there;
//   - a BASIC program: a listing or tokenized BASIC behind a header;
//   - machine code: behind a CE-158 / PC-1600 header, or headerless bytes the
//     library's heuristic takes for code (ProgramFile::looksLikeCode) in a
//     file named .bin / .rom. The heuristic alone also takes JPEGs, PDFs and
//     fonts for code, hence the name; the guess only admits the file, the
//     MODE still picks the CPU.
// Anything else is None, and the drop is ignored without a message. A
// damaged or mismatched file of a known kind still goes to its loader,
// which explains the refusal.

namespace dropfile {

enum class Target { None, Preset, BasicProgram, MachineCode };

/// `fileName` only matters for headerless machine code (its extension).
Target classify(const std::vector<uint8_t>& bytes, const std::string& fileName);

}  // namespace dropfile
