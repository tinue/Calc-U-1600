#import <Foundation/Foundation.h>

#if __has_attribute(swift_private)
#define AC_SWIFT_PRIVATE __attribute__((swift_private))
#else
#define AC_SWIFT_PRIVATE
#endif

/// The "pc1500" asset catalog image resource.
static NSString * const ACImageNamePc1500 AC_SWIFT_PRIVATE = @"pc1500";

/// The "pc1500A" asset catalog image resource.
static NSString * const ACImageNamePc1500A AC_SWIFT_PRIVATE = @"pc1500A";

/// The "pc1600" asset catalog image resource.
static NSString * const ACImageNamePc1600 AC_SWIFT_PRIVATE = @"pc1600";

#undef AC_SWIFT_PRIVATE
