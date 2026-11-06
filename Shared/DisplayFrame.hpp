#pragma once
#include <string>
#include <utility>
#include <vector>

namespace shared {

// One flat, model-agnostic frame a renderer paints from -- deliberately
// wider than either Core display type (PC1500Display's 156x7 vs
// PC1600DisplaySnapshot's 156x32, one StatusLine each) so the LCD view
// never needs to know which model is active.
struct DisplayFrame {
    int cols = 0;
    int rows = 0;
    std::vector<bool> pixels; // row-major, cols*rows entries
    bool poweredOn = true;    // PC1500Machine::isDisplayOn() / PC1600 clockEnabled
    std::vector<std::pair<std::string, bool>> statusSymbols;
};

} // namespace shared
