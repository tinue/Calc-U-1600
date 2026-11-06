#pragma once
#include <vector>

#include "Shared/KeyLayout/KeyLayoutTypes.hpp"
#include "Shared/MachineModel.hpp"

// Where the faceplate art, its key hit-regions and the LCD land in a pane
// of a given size: the art letterbox-fitted (aspect kept, centred), each
// KeyRect scaled from source-artwork pixels into pane pixels. Integer
// pane coordinates, truncated exactly as the Qt widget always did.
namespace KeyLayout {

const Layout& layoutFor(shared::Model model);

struct PaneRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    bool valid() const { return w > 0 && h > 0; }
};

struct FittedLayout {
    PaneRect art;                // the letterboxed art
    std::vector<PaneRect> keys;  // parallel to Layout::keys
    PaneRect lcd;
};

// Empty (art invalid, no keys) for a zero-sized pane.
FittedLayout fitLayout(const Layout& layout, int paneWidth, int paneHeight);

// Index into Layout::keys of the key under (x, y), or -1.
int hitTest(const FittedLayout& fitted, int x, int y);

// Index of the key named `name`, or -1.
int keyIndex(const Layout& layout, const char* name);

// heightForWidth for a pane that keeps the art's aspect ratio.
int heightForWidth(const Layout& layout, int width);

} // namespace KeyLayout
