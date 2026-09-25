#include "MacAppSupport.h"

#import <Foundation/Foundation.h>

void macDisableWindowRestoration() {
    @autoreleasepool {
        [[NSUserDefaults standardUserDefaults] registerDefaults:@{@"ApplePersistenceIgnoreState" : @YES}];
    }
}
