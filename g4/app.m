/*
 * app.m — "PowerPC Display.app" for Mac OS X 10.4 Tiger (G3, G4 or G5).
 *
 * A small Cocoa window around the display engine in client.c (compiled with
 * -DG4_APP). The engine runs on its own thread; this file only shows its
 * status and offers Pause/Resume, colour depth and Open at Login.
 *
 * While the other Mac is connected the engine takes over the whole screen.
 * Pressing Command-Escape pauses and brings this window back.
 *
 * Objective-C 1.0 only (no properties, no fast enumeration): Tiger's gcc 4.0.
 */
#import <Cocoa/Cocoa.h>
#include "g4client.h"

@interface G4AppDelegate : NSObject
{
    NSWindow    *window;
    NSTextField *titleField;
    NSTextField *detailField;
    NSButton    *pauseButton;
    NSPopUpButton *depthPopup;
    NSTextField *machineField;
    NSButton    *loginBox;
    int          state;
}
- (void)showStatus:(NSDictionary *)info;
@end

static G4AppDelegate *gDelegate = nil;

/* called on the engine thread: hand the update to the main thread */
static void status_hook(int st, const char *text)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSString *s = [NSString stringWithUTF8String:text];
    NSDictionary *info = [NSDictionary dictionaryWithObjectsAndKeys:
                          [NSNumber numberWithInt:st], @"state",
                          (s ? s : @""), @"text", nil];
    [gDelegate performSelectorOnMainThread:@selector(showStatus:) withObject:info waitUntilDone:NO];
    [pool release];
}

@implementation G4AppDelegate

- (NSTextField *)labelWithFrame:(NSRect)r size:(float)size bold:(BOOL)bold
{
    NSTextField *f = [[[NSTextField alloc] initWithFrame:r] autorelease];
    [f setEditable:NO];
    [f setSelectable:NO];
    [f setBezeled:NO];
    [f setDrawsBackground:NO];
    [f setFont:(bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size])];
    [[f cell] setWraps:YES];
    return f;
}

- (NSButton *)checkboxWithFrame:(NSRect)r title:(NSString *)title action:(SEL)action
{
    NSButton *b = [[[NSButton alloc] initWithFrame:r] autorelease];
    [b setButtonType:NSSwitchButton];
    [b setTitle:title];
    [b setTarget:self];
    [b setAction:action];
    return b;
}

- (void)buildMenu
{
    NSMenu *main = [[[NSMenu alloc] initWithTitle:@""] autorelease];
    NSMenuItem *appItem = [main addItemWithTitle:@"" action:NULL keyEquivalent:@""];
    NSMenu *appMenu = [[[NSMenu alloc] initWithTitle:@"PowerPC Display"] autorelease];
    NSMenuItem *winItem = [main addItemWithTitle:@"" action:NULL keyEquivalent:@""];
    NSMenu *winMenu = [[[NSMenu alloc] initWithTitle:@"Window"] autorelease];
    NSMenuItem *show;

    [appMenu addItemWithTitle:@"About PowerPC Display" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Hide PowerPC Display" action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItemWithTitle:@"Quit PowerPC Display" action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];

    [winMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [winMenu addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    show = [winMenu addItemWithTitle:@"PowerPC Display" action:@selector(showWindow:) keyEquivalent:@"0"];
    [show setTarget:self];
    [winItem setSubmenu:winMenu];

    [NSApp setMainMenu:main];
    /* Tiger needs this (undocumented there) for the app menu to behave */
    if ([NSApp respondsToSelector:@selector(setAppleMenu:)])
        [NSApp performSelector:@selector(setAppleMenu:) withObject:appMenu];
    [NSApp setWindowsMenu:winMenu];
}

- (void)buildWindow
{
    NSView *v;
    NSImageView *icon;
    NSTextField *label;

    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 460, 236)
                                         styleMask:(NSTitledWindowMask | NSClosableWindowMask | NSMiniaturizableWindowMask)
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
    [window setTitle:@"PowerPC Display"];
    [window setReleasedWhenClosed:NO];
    v = [window contentView];

    icon = [[[NSImageView alloc] initWithFrame:NSMakeRect(20, 152, 64, 64)] autorelease];
    [icon setImage:[NSApp applicationIconImage]];
    [v addSubview:icon];

    titleField = [self labelWithFrame:NSMakeRect(100, 192, 340, 22) size:14 bold:YES];
    [titleField setStringValue:@"Starting..."];
    [v addSubview:titleField];

    detailField = [self labelWithFrame:NSMakeRect(100, 128, 340, 60) size:11 bold:NO];
    [v addSubview:detailField];

    machineField = [self labelWithFrame:NSMakeRect(20, 96, 420, 16) size:10 bold:NO];
    [machineField setTextColor:[NSColor disabledControlTextColor]];
    [machineField setStringValue:[NSString stringWithFormat:@"This Mac: %s", g4c_machine_summary()]];
    [v addSubview:machineField];

    label = [self labelWithFrame:NSMakeRect(20, 64, 56, 18) size:13 bold:NO];
    [label setStringValue:@"Colour:"];
    [v addSubview:label];
    depthPopup = [[[NSPopUpButton alloc] initWithFrame:NSMakeRect(76, 58, 330, 26) pullsDown:NO] autorelease];
    [depthPopup addItemWithTitle:[NSString stringWithFormat:@"Automatic (%@ on this Mac)",
                                  (g4c_auto_depth() == 16 ? @"Thousands" : @"Millions")]];
    [depthPopup addItemWithTitle:@"Millions of colours (32-bit)"];
    [depthPopup addItemWithTitle:@"Thousands of colours (16-bit, less data)"];
    [depthPopup setTarget:self];
    [depthPopup setAction:@selector(changeDepth:)];
    [v addSubview:depthPopup];

    loginBox = [self checkboxWithFrame:NSMakeRect(20, 30, 300, 20)
                                 title:@"Open PowerPC Display at login"
                                action:@selector(toggleLogin:)];
    [v addSubview:loginBox];

    pauseButton = [[[NSButton alloc] initWithFrame:NSMakeRect(346, 14, 96, 32)] autorelease];
    [pauseButton setBezelStyle:NSRoundedBezelStyle];
    [pauseButton setTitle:@"Pause"];
    [pauseButton setTarget:self];
    [pauseButton setAction:@selector(togglePause:)];
    [v addSubview:pauseButton];

    [window center];
    [window setFrameAutosaveName:@"PowerPCDisplayWindow"];
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    int mode = [ud integerForKey:@"colourMode"];     /* 0 automatic, 1 32-bit, 2 16-bit */

    if (mode < 0 || mode > 2) mode = 0;
    [self buildMenu];
    [self buildWindow];
    [depthPopup selectItemAtIndex:mode];
    [loginBox setState:([ud boolForKey:@"openAtLogin"] ? NSOnState : NSOffState)];
    [window makeKeyAndOrderFront:nil];

    g4c_set_app_mode(1);
    g4c_set_depth(mode == 1 ? 32 : mode == 2 ? 16 : 0);
    g4c_set_status_hook(status_hook);
    [NSThread detachNewThreadSelector:@selector(engineThread:) toTarget:self withObject:nil];
}

- (void)engineThread:(id)unused
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    g4c_server_run();
    [pool release];
}

- (void)showStatus:(NSDictionary *)info
{
    int prev = state;
    state = [[info objectForKey:@"state"] intValue];
    switch (state) {
    case G4C_WAITING:   [titleField setStringValue:@"Waiting for your Mac"]; break;
    case G4C_CONNECTED: [titleField setStringValue:@"Showing your Mac"];     break;
    case G4C_PAUSED:    [titleField setStringValue:@"Paused"];               break;
    default:            [titleField setStringValue:@"Something's wrong"];    break;
    }
    [detailField setStringValue:[info objectForKey:@"text"]];
    [pauseButton setTitle:(state == G4C_PAUSED ? @"Resume" : @"Pause")];

    /* coming back from full screen: bring the window forward */
    if (prev == G4C_CONNECTED && state != G4C_CONNECTED) {
        [NSApp activateIgnoringOtherApps:YES];
        [window makeKeyAndOrderFront:nil];
    }
}

- (void)togglePause:(id)sender
{
    g4c_set_paused(!g4c_is_paused());
}

- (void)changeDepth:(id)sender
{
    int mode = [depthPopup indexOfSelectedItem];
    [[NSUserDefaults standardUserDefaults] setInteger:mode forKey:@"colourMode"];
    g4c_set_depth(mode == 1 ? 32 : mode == 2 ? 16 : 0);    /* used from the next connection */
}

/* Tiger has no login-item API; System Events does it */
- (BOOL)runScript:(NSString *)src
{
    NSDictionary *err = nil;
    NSAppleScript *as = [[[NSAppleScript alloc] initWithSource:src] autorelease];
    [as executeAndReturnError:&err];
    if (err) NSLog(@"PowerPC Display: login item script failed: %@", err);
    return err == nil;
}

- (void)toggleLogin:(id)sender
{
    BOOL on = ([loginBox state] == NSOnState);
    NSString *path = [[NSBundle mainBundle] bundlePath];
    NSString *name = [[path lastPathComponent] stringByDeletingPathExtension];
    NSString *src;
    BOOL ok;

    if (on)
        src = [NSString stringWithFormat:
               @"tell application \"System Events\"\n"
               @"  if not (exists login item \"%@\") then make login item at end with properties {path:\"%@\", hidden:false}\n"
               @"end tell", name, path];
    else
        src = [NSString stringWithFormat:
               @"tell application \"System Events\"\n"
               @"  if exists login item \"%@\" then delete login item \"%@\"\n"
               @"end tell", name, name];
    ok = [self runScript:src];
    if (!ok) [loginBox setState:(on ? NSOffState : NSOnState)];   /* undo the click */
    else [[NSUserDefaults standardUserDefaults] setBool:on forKey:@"openAtLogin"];
}

- (void)showWindow:(id)sender
{
    [window makeKeyAndOrderFront:nil];
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)app hasVisibleWindows:(BOOL)flag
{
    [window makeKeyAndOrderFront:nil];   /* clicking the Dock icon brings the window back */
    return YES;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    return NO;   /* keep listening for the other Mac with the window closed */
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    g4c_stop();
}

@end

int main(int argc, char **argv)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSApplication *app = [NSApplication sharedApplication];
    gDelegate = [[G4AppDelegate alloc] init];
    [app setDelegate:gDelegate];
    [app run];
    [pool release];
    return 0;
}
