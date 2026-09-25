#include "MacShotSupport.h"

#include <QWidget>

#import <Cocoa/Cocoa.h>
#include <unistd.h>

long macWindowNumber(const QWidget* widget) {
    if (!widget) return 0;
    const QWidget* window = widget->window();
    if (!window->windowHandle()) return 0;
    NSView* view = reinterpret_cast<NSView*>(window->winId());
    return view.window ? static_cast<long>(view.window.windowNumber) : 0;
}

QRect macOwnWindowsBounds(bool menusOnly) {
    QRect bounds;
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
                                                 kCGNullWindowID);
    if (!list) return bounds;
    const pid_t self = getpid();
    for (NSDictionary* info in (__bridge NSArray*)list) {
        if ([info[(id)kCGWindowOwnerPID] intValue] != self) continue;
        if (menusOnly && [info[(id)kCGWindowLayer] intValue] <= 0) continue;
        CGRect rect;
        if (!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)info[(id)kCGWindowBounds], &rect))
            continue;
        if (rect.size.width < 2 || rect.size.height < 2) continue;
        // CGWindow bounds are global, top-left origin, in points -- the same
        // space Qt uses for global coordinates on macOS.
        bounds |= QRect(qRound(rect.origin.x), qRound(rect.origin.y), qRound(rect.size.width),
                        qRound(rect.size.height));
    }
    CFRelease(list);
    return bounds;
}

void macActivateApp() {
    [NSApp activateIgnoringOtherApps:YES];
}
