#pragma once
#include <cstddef>

// Shared shape for the two per-model key-hit-region tables below: each
// entry is a key's named-key-vocabulary string (PC1500Keyboard::keyFromName
// / PC1600Keyboard::keyFromName) plus its hit rectangle in raw
// source-artwork pixels. The fraction-of-image-size conversion happens in
// FaceplateWidget at paint/hit-test time (imageWidth/imageHeight below is
// what to divide by), not baked in here -- kept as raw pixels so this table
// is a direct, eyeball-checkable transcription of the source artwork's own
// coordinates.
namespace KeyLayout {

struct KeyRect {
    const char* name;
    double x, y, w, h; // pixels, in the source artwork's own coordinate space
};

struct Layout {
    double imageWidth;
    double imageHeight;
    KeyRect lcdRect;
    const KeyRect* keys;
    std::size_t keyCount;
};

} // namespace KeyLayout
