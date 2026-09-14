#pragma once
#include <QImage>

#ifdef Q_OS_MACOS
// Writes `source` to the general pasteboard as an NSImage sized to
// widthPt x heightPt (Cocoa points, 1/72in). Needed because Qt's
// cross-platform QClipboard::setImage()/setPixmap() silently ignore both
// QImage::devicePixelRatio() and setDotsPerMeterX/Y() when converting to
// NSImage on macOS -- confirmed empirically: the pasted image always reports
// 72 DPI / a 1:1 pixel-to-point size no matter what, so pasting or printing
// "at 100%" would render roughly 8x too large for a 600 DPI render. This
// bypasses Qt's clipboard bridge entirely via
// `NSPasteboard.general.writeObjects([nsImage])`: the resulting
// NSBitmapImageRep carries every source pixel (so print quality is
// unaffected), but the NSImage's/rep's own `size` is set to the true
// physical dimensions in points, which is what AppKit consumers (Preview,
// the print system) actually read to determine physical size.
//
// Returns false (nothing written) if `source` is empty/invalid -- caller
// should fall back to Qt's own clipboard path in that case.
bool macSetClipboardImage(const QImage& source, double widthPt, double heightPt);
#endif
