#import "G4VirtualDisplay.h"

/*
 * CGVirtualDisplay is a private CoreGraphics API (macOS 11+). It is what
 * BetterDisplay, DisplayLink-style tools and AirPlay receivers use to create
 * displays that the window server treats as real monitors. The declarations
 * below are the commonly used reverse-engineered interface; classes are looked
 * up at runtime so a missing API fails gracefully instead of at link time.
 */
@interface CGVirtualDisplayDescriptor : NSObject
@property (retain, nonatomic) dispatch_queue_t queue;
@property (retain, nonatomic) NSString *name;
@property (nonatomic) unsigned int maxPixelsHigh;
@property (nonatomic) unsigned int maxPixelsWide;
@property (nonatomic) CGSize sizeInMillimeters;
@property (nonatomic) unsigned int serialNum;
@property (nonatomic) unsigned int productID;
@property (nonatomic) unsigned int vendorID;
@property (copy, nonatomic) void (^terminationHandler)(id, id);
@end

@interface CGVirtualDisplayMode : NSObject
- (instancetype)initWithWidth:(unsigned int)width height:(unsigned int)height refreshRate:(double)refreshRate;
@end

@interface CGVirtualDisplaySettings : NSObject
@property (retain, nonatomic) NSArray *modes;
@property (nonatomic) unsigned int hiDPI;
@end

@interface CGVirtualDisplay : NSObject
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor *)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings *)settings;
@property (readonly, nonatomic) unsigned int displayID;
@end

@implementation G4VirtualDisplay {
    id _display;   // strong: the display disappears when this is released
}

- (instancetype)initWithWidth:(unsigned)width height:(unsigned)height refreshRate:(double)hz name:(NSString *)name
                      widthMM:(double)widthMM heightMM:(double)heightMM
{
    self = [super init];
    if (!self) return nil;

    Class descCls = NSClassFromString(@"CGVirtualDisplayDescriptor");
    Class modeCls = NSClassFromString(@"CGVirtualDisplayMode");
    Class setCls  = NSClassFromString(@"CGVirtualDisplaySettings");
    Class dispCls = NSClassFromString(@"CGVirtualDisplay");
    if (!descCls || !modeCls || !setCls || !dispCls) return nil;

    CGVirtualDisplayDescriptor *desc = [[descCls alloc] init];
    desc.queue = dispatch_get_main_queue();
    desc.name = name;
    desc.maxPixelsWide = width;
    desc.maxPixelsHigh = height;
    // the PowerPC Mac's real panel size, so macOS picks sensible UI scaling
    desc.sizeInMillimeters = CGSizeMake(widthMM, heightMM);
    desc.vendorID = 0x0610;      // Apple
    desc.productID = 0x5050;     // 'PP'
    desc.serialNum = (uint32_t)(width << 16 | height);  // one identity per screen size
    desc.terminationHandler = ^(id a, id b) { NSLog(@"virtual display terminated"); };

    CGVirtualDisplay *display = [[dispCls alloc] initWithDescriptor:desc];
    if (!display) return nil;

    CGVirtualDisplaySettings *settings = [[setCls alloc] init];
    settings.hiDPI = 0;          // 1 point = 1 pixel: exactly what the PowerPC Mac shows
    settings.modes = @[ [[modeCls alloc] initWithWidth:width height:height refreshRate:hz] ];
    if (![display applySettings:settings]) return nil;

    _display = display;
    return self;
}

- (CGDirectDisplayID)displayID
{
    return [(CGVirtualDisplay *)_display displayID];
}

@end
