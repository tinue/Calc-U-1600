#include "MacClipboardImage.h"

#import <Cocoa/Cocoa.h>
#include <cstring>

bool macSetClipboardImage(const QImage& source, double widthPt, double heightPt) {
    if (source.isNull() || source.width() <= 0 || source.height() <= 0) return false;
    if (widthPt <= 0 || heightPt <= 0) return false;

    // Format_RGBA8888 has a platform-independent byte order (R,G,B,A per
    // pixel) matching NSBitmapImageRep's expected non-premultiplied RGBA
    // layout -- avoids the endianness ambiguity of Format_ARGB32's packed
    // 32-bit value.
    const QImage image = source.convertToFormat(QImage::Format_RGBA8888);
    const int w = image.width();
    const int h = image.height();

    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL
        pixelsWide:w
        pixelsHigh:h
        bitsPerSample:8
        samplesPerPixel:4
        hasAlpha:YES
        isPlanar:NO
        colorSpaceName:NSDeviceRGBColorSpace
        bytesPerRow:0
        bitsPerPixel:0];
    if (!rep) return false;

    unsigned char* dst = [rep bitmapData];
    const NSInteger dstStride = [rep bytesPerRow];
    for (int y = 0; y < h; ++y) {
        std::memcpy(dst + y * dstStride, image.constScanLine(y), static_cast<size_t>(w) * 4);
    }
    [rep setSize:NSMakeSize(widthPt, heightPt)];

    NSImage* nsImage = [[NSImage alloc] initWithSize:NSMakeSize(widthPt, heightPt)];
    [nsImage addRepresentation:rep];

    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    return [pasteboard writeObjects:@[nsImage]] == YES;
}
