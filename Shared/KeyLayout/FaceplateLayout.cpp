#include "Shared/KeyLayout/FaceplateLayout.hpp"

#include <cstring>

#include "Shared/KeyLayout/PC1500KeyLayout.hpp"
#include "Shared/KeyLayout/PC1600KeyLayout.hpp"

namespace KeyLayout {

const Layout& layoutFor(shared::Model model) {
    // PC-1500 and PC-1500A share the art apart from the model-name text.
    return model == shared::Model::PC1600 ? pc1600Layout() : pc1500Layout();
}

FittedLayout fitLayout(const Layout& layout, int paneWidth, int paneHeight) {
    FittedLayout out;
    if (paneWidth <= 0 || paneHeight <= 0) return out;

    // Letterbox-fit: largest centered rect preserving the model's aspect
    // ratio within the pane.
    const double imageAspect = layout.imageWidth / layout.imageHeight;
    const double paneAspect = static_cast<double>(paneWidth) / paneHeight;
    int fitW, fitH;
    if (paneAspect > imageAspect) {
        fitH = paneHeight;
        fitW = static_cast<int>(fitH * imageAspect);
    } else {
        fitW = paneWidth;
        fitH = static_cast<int>(fitW / imageAspect);
    }
    out.art = PaneRect{(paneWidth - fitW) / 2, (paneHeight - fitH) / 2, fitW, fitH};

    const PaneRect& art = out.art;
    const auto toPane = [&](const KeyRect& r) {
        const double fx = r.x / layout.imageWidth;
        const double fy = r.y / layout.imageHeight;
        const double fw = r.w / layout.imageWidth;
        const double fh = r.h / layout.imageHeight;
        return PaneRect{art.x + static_cast<int>(fx * art.w), art.y + static_cast<int>(fy * art.h),
                        static_cast<int>(fw * art.w), static_cast<int>(fh * art.h)};
    };
    out.keys.reserve(layout.keyCount);
    for (std::size_t i = 0; i < layout.keyCount; ++i) out.keys.push_back(toPane(layout.keys[i]));
    out.lcd = toPane(layout.lcdRect);
    return out;
}

int hitTest(const FittedLayout& fitted, int x, int y) {
    for (std::size_t i = 0; i < fitted.keys.size(); ++i) {
        if (fitted.keys[i].contains(x, y)) return static_cast<int>(i);
    }
    return -1;
}

int keyIndex(const Layout& layout, const char* name) {
    for (std::size_t i = 0; i < layout.keyCount; ++i) {
        if (std::strcmp(layout.keys[i].name, name) == 0) return static_cast<int>(i);
    }
    return -1;
}

int heightForWidth(const Layout& layout, int width) {
    if (layout.imageWidth <= 0) return width;
    return static_cast<int>(width * (layout.imageHeight / layout.imageWidth));
}

} // namespace KeyLayout
