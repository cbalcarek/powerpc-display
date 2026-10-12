#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

NS_ASSUME_NONNULL_BEGIN

/// A virtual monitor that macOS treats as a real extended display.
/// Lives as long as this object is retained.
@interface G4VirtualDisplay : NSObject
- (nullable instancetype)initWithWidth:(unsigned)width
                                height:(unsigned)height
                           refreshRate:(double)hz
                                  name:(NSString *)name
                               widthMM:(double)widthMM
                              heightMM:(double)heightMM;
@property (nonatomic, readonly) CGDirectDisplayID displayID;
@end

NS_ASSUME_NONNULL_END
