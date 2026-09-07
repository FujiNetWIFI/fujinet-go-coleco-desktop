/*
 * ColecoAppDelegate -- the window, the menu bar and the session's lifetime.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "AppDelegate.h"

#import "ColecoKeyForward.h"
#import "DisplayView.h"
#import "debugger/DebuggerWindow.h"
#import "keypad/KeypadWindow.h"

#include <string.h>

/* The content view sits between AppKit and the session: it owns the held-key
 * set and forwards. Keeping it here rather than in DisplayView leaves the
 * display purely about pixels. */
@interface ColecoContentView : NSView
@property (nonatomic) colecosession *session;
@end

@implementation ColecoContentView {
    coleco_input_state _keys;
}

- (BOOL)acceptsFirstResponder { return YES; }

- (void)pushInput
{
    /* Push both ports every time: the controller word is a snapshot of
     * everything held, so sending only the port that changed would be an
     * optimisation with a bug in it the first time keys on both are down. */
    colecosession_joystick_raw(self.session, 0, coleco_input_word(&_keys, 0));
    colecosession_joystick_raw(self.session, 1, coleco_input_word(&_keys, 1));
}

- (void)keyDown:(NSEvent *)e
{
    if ([e isARepeat]) return;
    const uint32_t ks = ColecoKeysymFromEvent(e);
    if (!ks) return;

    const int sa = coleco_input_key_sysaction(ks);
    if (sa >= 0) { colecosession_sysaction(self.session, sa); return; }
    if (coleco_input_key(&_keys, ks, 1)) [self pushInput];
}

- (void)keyUp:(NSEvent *)e
{
    const uint32_t ks = ColecoKeysymFromEvent(e);
    if (ks && coleco_input_key(&_keys, ks, 0)) [self pushInput];
}

/* Modifier keys arrive as flagsChanged, not keyDown/keyUp, and both fire
 * buttons are modifiers in the default bindings -- so without this the
 * machine never sees a shot fired from the keyboard. */
- (void)flagsChanged:(NSEvent *)e
{
    const NSEventModifierFlags f = [e modifierFlags];
    coleco_input_key(&_keys, 0xFFE3, (f & NSEventModifierFlagControl) ? 1 : 0);
    coleco_input_key(&_keys, 0xFFE9, (f & NSEventModifierFlagOption) ? 1 : 0);
    coleco_input_key(&_keys, 0xFFE1, (f & NSEventModifierFlagShift) ? 1 : 0);
    [self pushInput];
}

- (void)releaseAll
{
    coleco_input_reset(&_keys);
    [self pushInput];
}
@end

@implementation ColecoAppDelegate {
    colecosession *_session;
    const char *_cartPath;
    NSWindow *_window;
    ColecoDisplayView *_display;
    ColecoContentView *_content;
    NSTextField *_status;
    NSTimer *_statusTimer;
}

- (instancetype)initWithSession:(colecosession *)session
                       cartPath:(const char *)cartPath
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _cartPath = cartPath;
    return self;
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;

    _window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 848, 636)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable |
                             NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_window setTitle:@"FujiNet Go ColecoVision"];
    [_window center];

    _content = [[ColecoContentView alloc]
        initWithFrame:[[_window contentView] bounds]];
    _content.session = _session;
    [_content setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

    _display = [[ColecoDisplayView alloc] initWithSession:_session];
    [_display setFrame:[_content bounds]];
    [_display setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_display setTvAspect:colecosession_get_int(_session, "tv_aspect", 1) != 0];
    [_display setSmooth:colecosession_get_int(_session, "smooth", 0) != 0];
    [_content addSubview:_display];

    [_window setContentView:_content];
    [self buildMenu];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_content];
    [_window setDelegate:(id<NSWindowDelegate>)self];
    [_window registerForDraggedTypes:@[NSPasteboardTypeFileURL]];

    colecosession_start_opts opts;
    colecosession_default_opts(_session, &opts);
    if (_cartPath) opts.cart_path = _cartPath;
    if (colecosession_start(_session, &opts) != 0) {
        /* Show the window anyway: the usual reason to be here is a fresh
         * install with no BIOS, and the window is where the import lives. */
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Could not start"];
        [a setInformativeText:
            [NSString stringWithUTF8String:colecosession_last_error(_session)]];
        [a runModal];
    }

    /* The family's launch hooks, for when the app misbehaves before a menu
     * is reachable. */
    if (getenv("COLECO_OPEN_KEYPAD"))
        [ColecoKeypadWindow toggleWithSession:_session];
    if (getenv("COLECO_OPEN_DEBUGGER"))
        [DebuggerWindow showForSession:_session];

    _statusTimer = [NSTimer scheduledTimerWithTimeInterval:1.0
        repeats:YES block:^(NSTimer *t) { (void)t; [self updateTitle]; }];
    [self updateTitle];
}

- (void)updateTitle
{
    const char *state;
    if (!colecosession_is_running(_session))                state = "Stopped";
    else if (!colecosession_cart_mailbox_live(_session))    state = "No FujiNet cartridge";
    else if (colecosession_cart_link_up(_session))          state = "FujiNet connected";
    else                                                    state = "FujiNet: link down";
    /* The title bar is the status bar here: an AppKit window has no natural
     * place for one, and a floating HUD over the picture would be worse. */
    [_window setTitle:[NSString stringWithFormat:@"FujiNet Go ColecoVision — %s",
                       state]];
}

/* Losing key status with keys held would leave the machine believing they are
 * still down. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [_content releaseAll];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_statusTimer invalidate];
    [_display stop];
    colecosession_stop(_session);
    colecosession_free(_session);
}

/* ---- menu ---------------------------------------------------------------- */

- (void)buildMenu
{
    NSMenu *bar = [[NSMenu alloc] init];

    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"About FujiNet Go ColecoVision"
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:)
                keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
    [bar addItem:appItem];

    NSMenuItem *machineItem = [[NSMenuItem alloc] init];
    NSMenu *machine = [[NSMenu alloc] initWithTitle:@"Machine"];
    [[machine addItemWithTitle:@"Open Cartridge…" action:@selector(openCart:)
                 keyEquivalent:@"o"] setTarget:self];
    [[machine addItemWithTitle:@"Eject Cartridge" action:@selector(ejectCart:)
                 keyEquivalent:@""] setTarget:self];
    [machine addItem:[NSMenuItem separatorItem]];
    [[machine addItemWithTitle:@"Reset Console" action:@selector(resetConsole:)
                 keyEquivalent:@"r"] setTarget:self];
    [[machine addItemWithTitle:@"Reset to CONFIG" action:@selector(resetConfig:)
                 keyEquivalent:@""] setTarget:self];
    [machine addItem:[NSMenuItem separatorItem]];
    [[machine addItemWithTitle:@"Import BIOS…" action:@selector(importBios:)
                 keyEquivalent:@""] setTarget:self];
    [machineItem setSubmenu:machine];
    [bar addItem:machineItem];

    NSMenuItem *viewItem = [[NSMenuItem alloc] init];
    NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
    [[view addItemWithTitle:@"Controllers" action:@selector(toggleKeypad:)
              keyEquivalent:@"k"] setTarget:self];
    [[view addItemWithTitle:@"Debugger" action:@selector(showDebugger:)
              keyEquivalent:@"d"] setTarget:self];
    [view addItem:[NSMenuItem separatorItem]];
    NSMenuItem *tv = [view addItemWithTitle:@"TV Aspect (4:3)"
                                     action:@selector(toggleAspect:)
                              keyEquivalent:@""];
    [tv setTarget:self];
    [tv setState:(colecosession_get_int(_session, "tv_aspect", 1)
                  ? NSControlStateValueOn : NSControlStateValueOff)];
    NSMenuItem *sm = [view addItemWithTitle:@"Smooth Scaling"
                                     action:@selector(toggleSmooth:)
                              keyEquivalent:@""];
    [sm setTarget:self];
    [sm setState:(colecosession_get_int(_session, "smooth", 0)
                  ? NSControlStateValueOn : NSControlStateValueOff)];
    [viewItem setSubmenu:view];
    [bar addItem:viewItem];

    NSMenuItem *fujiItem = [[NSMenuItem alloc] init];
    NSMenu *fuji = [[NSMenu alloc] initWithTitle:@"FujiNet"];
    [[fuji addItemWithTitle:@"Configuration" action:@selector(openWebUI:)
              keyEquivalent:@""] setTarget:self];
    [fujiItem setSubmenu:fuji];
    [bar addItem:fujiItem];

    [NSApp setMainMenu:bar];
}

/* ---- actions ------------------------------------------------------------- */

- (void)loadMedia:(NSString *)path
{
    char dest[1024];
    if (colecosession_import_media(_session, [path UTF8String], dest,
                                   sizeof dest) != 0) {
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Import failed"];
        [a setInformativeText:
            [NSString stringWithUTF8String:colecosession_last_error(_session)]];
        [a runModal];
        return;
    }
    if (!colecosession_media_is_cartridge(dest)) {
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Imported"];
        [a setInformativeText:@"Copied to FujiNet's SD folder. "
                               "Mount it from the CONFIG client."];
        [a runModal];
        return;
    }
    colecosession_set_str(_session, "cart_path", dest);
    colecosession_settings_flush(_session);

    colecosession_start_opts o;
    colecosession_default_opts(_session, &o);
    colecosession_stop(_session);
    colecosession_start(_session, &o);
}

- (void)openCart:(id)sender
{
    (void)sender;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    [p setAllowedFileTypes:@[@"rom", @"col", @"bin"]];
    if ([p runModal] == NSModalResponseOK)
        [self loadMedia:[[p URL] path]];
}

- (void)ejectCart:(id)sender
{
    (void)sender;
    colecosession_set_str(_session, "cart_path", "");
    colecosession_settings_flush(_session);
    colecosession_reset_to_config(_session);
}

- (void)resetConsole:(id)sender { (void)sender; colecosession_reset(_session); }
- (void)resetConfig:(id)sender
{
    (void)sender;
    colecosession_reset_to_config(_session);
}

- (void)importBios:(id)sender
{
    (void)sender;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    [p setAllowedFileTypes:@[@"rom", @"bin"]];
    if ([p runModal] != NSModalResponseOK) return;
    NSAlert *a = [[NSAlert alloc] init];
    if (colecosession_import_bios(_session, [[[p URL] path] UTF8String]) < 0) {
        [a setMessageText:@"Import failed"];
        [a setInformativeText:
            [NSString stringWithUTF8String:colecosession_last_error(_session)]];
    } else {
        [a setMessageText:@"BIOS imported"];
        [a setInformativeText:@"Restart to boot it."];
    }
    [a runModal];
}

- (void)toggleKeypad:(id)sender
{
    (void)sender;
    [ColecoKeypadWindow toggleWithSession:_session];
}

- (void)showDebugger:(id)sender
{
    (void)sender;
    [DebuggerWindow showForSession:_session];
}

- (void)toggleAspect:(id)sender
{
    NSMenuItem *item = sender;
    const BOOL on = ([item state] != NSControlStateValueOn);
    [item setState:(on ? NSControlStateValueOn : NSControlStateValueOff)];
    [_display setTvAspect:on];
    colecosession_set_int(_session, "tv_aspect", on ? 1 : 0);
}

- (void)toggleSmooth:(id)sender
{
    NSMenuItem *item = sender;
    const BOOL on = ([item state] != NSControlStateValueOn);
    [item setState:(on ? NSControlStateValueOn : NSControlStateValueOff)];
    [_display setSmooth:on];
    colecosession_set_int(_session, "smooth", on ? 1 : 0);
}

- (void)openWebUI:(id)sender
{
    (void)sender;
    if (!colecosession_fujinet_running(_session)) {
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"FujiNet is not running"];
        [a runModal];
        return;
    }
    [[NSWorkspace sharedWorkspace] openURL:
        [NSURL URLWithString:
            [NSString stringWithUTF8String:
                colecosession_fujinet_webui_url(_session)]]];
}

/* ---- drag and drop ------------------------------------------------------- */

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    (void)sender;
    return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    NSArray *urls = [[sender draggingPasteboard]
        readObjectsForClasses:@[[NSURL class]] options:nil];
    if ([urls count] == 0) return NO;
    [self loadMedia:[(NSURL *)urls[0] path]];
    return YES;
}
@end
