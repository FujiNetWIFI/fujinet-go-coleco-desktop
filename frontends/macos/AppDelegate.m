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
#import "help/HelpWindow.h"
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
    NSWindow *_settingsWindow;
    NSTextField *_externalField;
    NSTextField *_biosState;   /* the Settings ROMs row; nil until opened */
    BOOL _sessionDirty;
    NSWindow *_logWindow;
    NSTextView *_logView;
    NSTimer *_logTimer;
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
    [_window setDelegate:self];
    [_window registerForDraggedTypes:@[NSPasteboardTypeFileURL]];

    colecosession_start_opts opts;
    colecosession_default_opts(_session, &opts);
    if (_cartPath) opts.cart_path = _cartPath;
    if (colecosession_start(_session, &opts) != 0) {
        /* Show the window anyway: the usual reason to be here is a fresh
         * install with no BIOS, and the window is where the import lives. */
        [self reportRefusal:@"Could not start"
                      error:[NSString stringWithUTF8String:
                                 colecosession_last_error(_session)]];
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
    [[appMenu addItemWithTitle:@"About FujiNet Go ColecoVision"
                        action:@selector(showAbout:)
                 keyEquivalent:@""] setTarget:self];
    [appMenu addItem:[NSMenuItem separatorItem]];
    /* Command-comma, where every Mac application puts its settings. */
    [[appMenu addItemWithTitle:@"Settings…" action:@selector(showSettings:)
                 keyEquivalent:@","] setTarget:self];
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
    [[machine addItemWithTitle:@"Import BIOS from a Folder…"
                        action:@selector(importBiosFolder:)
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
    [[fuji addItemWithTitle:@"Console Log"
                     action:@selector(showFujiNetLog:)
              keyEquivalent:@""] setTarget:self];
    [fujiItem setSubmenu:fuji];
    [bar addItem:fujiItem];

    NSMenuItem *helpItem = [[NSMenuItem alloc] init];
    NSMenu *help = [[NSMenu alloc] initWithTitle:@"Help"];
    /* openHelp:, not showHelp: (AppKit's: "Help isn't available") */
    [self item:help title:@"FujiNet Go ColecoVision Help" action:@selector(openHelp:) key:@"?"];
    [helpItem setSubmenu:help];
    [bar addItem:helpItem];

    [NSApp setMainMenu:bar];
    [NSApp setHelpMenu:help];
    /* the Help menu's search field finds the Help's sections too */
    [NSApp registerUserInterfaceItemSearchHandler:[COLECOHelpWindow shared]];
}

/* For the window in front: Settings, the debugger's tab ... */
- (void)openHelp:(id)sender
{
    (void)sender;
    [[COLECOHelpWindow shared] showFor:[NSApp keyWindow]];
}

/* ---- settings ------------------------------------------------------------
 *
 * Every option here is read by colecosession_default_opts() when the session
 * starts, so none of them can apply mid-run. Changes are written to the
 * shared settings store as they are made (which is what lets the GNOME, KDE
 * and Windows frontends see the same configuration) and collected into ONE
 * session restart when the window closes -- restarting on each click would
 * tear the machine down under someone still deciding.
 */

- (NSTextField *)sectionLabel:(NSString *)title
{
    NSTextField *label = [NSTextField labelWithString:title];
    label.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return label;
}

- (NSPopUpButton *)popUpForKey:(const char *)key
                      fallback:(int)def
                         items:(NSArray<NSString *> *)items
{
    NSPopUpButton *popup = [[NSPopUpButton alloc] init];
    [popup addItemsWithTitles:items];
    NSInteger current = colecosession_get_int(_session, key, def);
    if (current < 0 || current >= (NSInteger)items.count)
        current = def;
    [popup selectItemAtIndex:current];
    popup.identifier = @(key);
    popup.target = self;
    popup.action = @selector(settingChanged:);
    return popup;
}

- (NSButton *)checkBoxForKey:(const char *)key fallback:(int)def
{
    NSButton *box = [NSButton checkboxWithTitle:@""
                                         target:self
                                         action:@selector(settingChanged:)];
    box.state = colecosession_get_int(_session, key, def)
                    ? NSControlStateValueOn
                    : NSControlStateValueOff;
    box.identifier = @(key);
    return box;
}

- (void)settingChanged:(id)sender
{
    NSControl *control = sender;
    NSString *key = control.identifier;
    int value;

    if ([control isKindOfClass:[NSPopUpButton class]])
        value = (int)((NSPopUpButton *)control).indexOfSelectedItem;
    else
        value = ((NSButton *)control).state == NSControlStateValueOn ? 1 : 0;

    colecosession_set_int(_session, [key UTF8String], value);
    _sessionDirty = YES;
    if ([key isEqualToString:@"fujinet_external"])
        _externalField.enabled = value != 0;
}

/* The external-FujiNet address field: valid text (or none, meaning
 * 127.0.0.1:11502) is saved; anything else is shown in red and not saved. */
- (void)externalAddrChanged:(id)sender
{
    NSTextField *field = sender;
    NSString *text = [field.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
    char host[64];
    int port;
    if (colecosession_parse_hostport(text.UTF8String, host, sizeof host,
                                     &port) != 0) {
        field.textColor = NSColor.systemRedColor;
        return;
    }
    field.textColor = NSColor.controlTextColor;
    colecosession_set_str(_session, "fujinet_external_addr", text.UTF8String);
    _sessionDirty = YES;
}

- (void)showAbout:(id)sender
{
    (void)sender;
    NSMutableAttributedString *credits = [[NSMutableAttributedString alloc]
        initWithString:@"Self-contained ColecoVision with built-in FujiNet.\n"
                        "GPL-3.0-or-later\n"];
    NSAttributedString *link = [[NSAttributedString alloc]
        initWithString:@"fujinet.online"
            attributes:@{ NSLinkAttributeName :
                              [NSURL URLWithString:@"https://fujinet.online/"] }];
    [credits appendAttributedString:link];
    [NSApp orderFrontStandardAboutPanelWithOptions:
        @{ NSAboutPanelOptionCredits : credits }];
}

- (void)showSettings:(id)sender
{
    (void)sender;
    if (_settingsWindow) {
        [_settingsWindow makeKeyAndOrderFront:nil];
        return;
    }

    _externalField = [NSTextField textFieldWithString:@(
        colecosession_get_str(_session, "fujinet_external_addr", ""))];
    _externalField.placeholderString = @"127.0.0.1:11502";
    _externalField.preferredMaxLayoutWidth = 240;
    _externalField.enabled =
        colecosession_get_int(_session, "fujinet_external", 0) != 0;
    _externalField.target = self;
    _externalField.action = @selector(externalAddrChanged:);
    [_externalField.cell setSendsActionOnEndEditing:YES];

    _biosState = [NSTextField labelWithString:@""];
    [self refreshRoms];

    NSArray<NSArray<NSView *> *> *rows = @[
        @[ [self sectionLabel:@"Machine"], [NSTextField labelWithString:@""] ],
        @[
            [NSTextField labelWithString:@"Super Game Module"],
            [self checkBoxForKey:"sgm" fallback:1]
        ],
        @[
            [NSTextField labelWithString:@"Palette"],
            [self popUpForKey:"palette"
                     fallback:0
                        items:@[
                            @"Default (TMS9928A)", @"Palette 2", @"Palette 3",
                            @"Palette 4"
                        ]]
        ],
        @[
            [NSTextField labelWithString:@"Swap controller buttons"],
            [self checkBoxForKey:"swap_buttons" fallback:0]
        ],
        @[ [self sectionLabel:@"ROMs"], [NSTextField labelWithString:@""] ],
        @[ [NSTextField labelWithString:@"Import"], [self romButtons] ],
        @[ [NSTextField labelWithString:@"Kept in"], [self romPathLabel] ],
        @[ [NSTextField labelWithString:@"OS7.rom"], _biosState ],
        @[ [self sectionLabel:@"FujiNet"], [NSTextField labelWithString:@""] ],
        @[
            [NSTextField labelWithString:@"Enable FujiNet"],
            [self checkBoxForKey:"enable_fujinet" fallback:1]
        ],
        @[
            [NSTextField labelWithString:@"Use external FujiNet"],
            [self checkBoxForKey:"fujinet_external" fallback:0]
        ],
        @[ [NSTextField labelWithString:@"Address:port"], _externalField ],
        @[ [self sectionLabel:@"Host"], [NSTextField labelWithString:@""] ],
        @[
            [NSTextField labelWithString:@"Audio"],
            [self checkBoxForKey:"enable_audio" fallback:1]
        ],
        @[
            [NSTextField labelWithString:@"Gamepads"],
            [self checkBoxForKey:"enable_gamepad" fallback:1]
        ],
    ];

    NSGridView *grid = [NSGridView gridViewWithViews:rows];
    grid.rowSpacing = 8;
    grid.columnSpacing = 12;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;

    NSTextField *note = [NSTextField
        labelWithString:@"Applied by restarting the session when this window "
                        @"is closed."];
    note.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    note.textColor = NSColor.secondaryLabelColor;

    NSStackView *root = [NSStackView stackViewWithViews:@[ grid, note ]];
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 12;
    root.edgeInsets = NSEdgeInsetsMake(16, 16, 16, 16);

    _settingsWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 460, 430)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _settingsWindow.title = @"Settings";
    /* Not released on close: reopening keeps the window's position, and the
     * delegate below still needs to be able to identify it. */
    _settingsWindow.releasedWhenClosed = NO;
    _settingsWindow.delegate = self;
    {
        /* the settings over a strip with the Help button */
        NSButton *helpBtn = COLECOHelpButton(@"prefs/Machine");
        NSStackView *wrap = [NSStackView stackViewWithViews:@[root, helpBtn]];
        wrap.orientation = NSUserInterfaceLayoutOrientationVertical;
        wrap.alignment = NSLayoutAttributeLeading;
        wrap.edgeInsets = NSEdgeInsetsMake(0, 16, 12, 16);
        _settingsWindow.contentView = wrap;
        COLECOSetHelpContext(_settingsWindow, @"prefs/Machine");
    }
    [_settingsWindow center];
    [_settingsWindow makeKeyAndOrderFront:nil];
}

/* The Settings ROMs section: two buttons, the folder the ROMs are kept in and
 * whether OS7.rom is there. Importing applies at once, not on close. */
- (NSView *)romButtons
{
    NSButton *files = [NSButton buttonWithTitle:@"Import BIOS…"
                                         target:self
                                         action:@selector(importBios:)];
    NSButton *folder = [NSButton buttonWithTitle:@"Import from a Folder…"
                                          target:self
                                          action:@selector(importBiosFolder:)];
    NSStackView *row = [NSStackView stackViewWithViews:@[ files, folder ]];
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.spacing = 8;
    return row;
}

- (NSTextField *)romPathLabel
{
    NSTextField *l = [NSTextField labelWithString:
        [NSString stringWithUTF8String:colecosession_roms_path(_session)]];
    l.selectable = YES;
    l.lineBreakMode = NSLineBreakByTruncatingMiddle;
    l.preferredMaxLayoutWidth = 260;
    l.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    l.textColor = NSColor.secondaryLabelColor;
    return l;
}

- (void)refreshRoms
{
    if (!_biosState) return;
    const BOOL have = colecosession_bios_available(_session) != 0;
    _biosState.stringValue = have ? @"Imported" : @"Not imported";
    _biosState.textColor =
        have ? NSColor.controlTextColor : NSColor.secondaryLabelColor;
}

- (void)windowWillClose:(NSNotification *)note
{
    if (note.object == _logWindow) {
        /* Otherwise the timer keeps firing against a hidden view forever. */
        [_logTimer invalidate];
        _logTimer = nil;
        return;
    }
    if (note.object == _settingsWindow) {
        [_settingsWindow makeFirstResponder:nil]; /* commit a field edit */
        _biosState = nil;
    }
    if (note.object != _settingsWindow || !_sessionDirty)
        return;
    _sessionDirty = NO;

    colecosession_start_opts o;
    colecosession_settings_flush(_session);
    colecosession_default_opts(_session, &o);
    colecosession_stop(_session);
    colecosession_start(_session, &o);
}

/* ---- actions ------------------------------------------------------------- */

- (void)loadMedia:(NSString *)path
{
    char dest[1024];
    /* A BIOS dump is a ROM, not a cartridge, though it is a .rom or .bin. */
    if (colecosession_media_is_rom([path UTF8String])) {
        [self importRomPaths:@[ path ]];
        return;
    }
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
    if (colecosession_start(_session, &o) != 0)
        [self reportRefusal:@"Could not open"
                      error:[NSString stringWithUTF8String:
                                 colecosession_last_error(_session)]];
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

/* ---- Import BIOS -----------------------------------------------------------
 * Like the Odyssey 2 and MS-DOS desktops: files (several at once, zips
 * included) or a folder, one result alert, and a power cycle when the import
 * supplied the BIOS the machine was waiting for. */

- (void)alert:(NSString *)title text:(NSString *)text
{
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:title];
    [a setInformativeText:text];
    [a runModal];
}

/* "Import BIOS…" / "Not Now". Choosing Import opens the files chooser. */
- (void)askForRoms:(NSString *)title text:(NSString *)text
{
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:title];
    [a setInformativeText:text];
    [a addButtonWithTitle:@"Import BIOS…"];
    [a addButtonWithTitle:@"Not Now"];
    a.showsHelp = YES;
    a.delegate = self;
    if ([a runModal] == NSAlertFirstButtonReturn)
        [self importRomsFromPanel:NO];
}

/* A refusal whose text says to import the BIOS gets the offer; anything else
 * is a plain alert. */
- (void)reportRefusal:(NSString *)title error:(NSString *)error
{
    if ([error rangeOfString:@"Import BIOS"].location != NSNotFound)
        [self askForRoms:title text:error];
    else
        [self alert:title text:error];
}

- (void)importRomsFromPanel:(BOOL)folder
{
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseDirectories = folder;
    p.canChooseFiles = !folder;
    p.allowsMultipleSelection = !folder;
    p.message = folder
        ? @"Import BIOS from a Folder (MAME's ROM folder works)"
        : @"Import BIOS: OS7.rom, or a zip (MAME's coleco.zip works)";
    if ([p runModal] != NSModalResponseOK) return;

    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    for (NSURL *u in p.URLs)
        [paths addObject:u.path];
    [self importRomPaths:paths];
}

/* Imports every path in turn and says what happened, always. Returns the
 * number of items taken. */
- (int)importRomPaths:(NSArray<NSString *> *)paths
{
    const BOOL wasMissing =
        !colecosession_bios_available(_session) ||
        !colecosession_is_running(_session);
    NSMutableString *log = [NSMutableString string];
    int total = 0;

    for (NSString *path in paths) {
        char msg[2048];
        const int rc = colecosession_rom_import(_session, path.UTF8String,
                                                msg, sizeof msg);
        if (rc > 0) total += rc;
        if (msg[0]) {
            if (log.length) [log appendString:@"\n"];
            [log appendString:[NSString stringWithUTF8String:msg]];
        }
    }
    [self refreshRoms];

    if (total > 0 && wasMissing) {
        colecosession_start_opts o;
        colecosession_default_opts(_session, &o);
        colecosession_stop(_session);
        if (colecosession_start(_session, &o) == 0) {
            [log appendString:@"\n\nThe console was power-cycled with the "
                              @"new ROMs."];
        } else {
            [log appendFormat:@"\n\n%s", colecosession_last_error(_session)];
        }
    }
    [self alert:(total > 0 ? @"BIOS Imported" : @"Nothing Imported")
           text:(log.length ? log : @"None of these files is a ROM this app uses.")];
    return total;
}

- (void)importBios:(id)sender
{
    (void)sender;
    [self importRomsFromPanel:NO];
}

- (void)importBiosFolder:(id)sender
{
    (void)sender;
    [self importRomsFromPanel:YES];
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

/* ---- FujiNet console log ---------------------------------------------------
 *
 * A live view of the in-process runtime's captured output. This project is a
 * protocol bring-up as much as an emulator -- the cartridge dials into the
 * FujiNet and every transaction is logged -- so being able to watch that
 * without leaving the app is the difference between "it does not work" and
 * knowing which command failed.
 */

- (void)refreshLog:(NSTimer *)timer
{
    (void)timer;
    /* Static: the ring is large and this runs once a second. */
    static char buf[128 * 1024];
    const int n = colecosession_fujinet_copy_log(_session, buf, sizeof buf);
    NSScrollView *scroll = (NSScrollView *)_logView.enclosingScrollView;
    /* Follow the tail only while the reader is already at the bottom;
     * scrolling up to read something must not be yanked away a second
     * later. */
    const BOOL atEnd =
        !scroll || (NSMaxY(scroll.contentView.documentVisibleRect) >=
                    NSMaxY(((NSView *)scroll.documentView).frame) - 4.0);

    [_logView setString:(n > 0 ? [NSString stringWithUTF8String:buf]
                               : @"(no FujiNet output yet)")];
    if (atEnd)
        [_logView scrollRangeToVisible:NSMakeRange(_logView.string.length, 0)];
}

/* The alert's Help button opens the nobios topic. */
- (BOOL)alertShowHelp:(NSAlert *)alert
{
    (void)alert;
    [[COLECOHelpWindow shared] showAt:@"nobios"];
    return YES;
}

- (void)showFujiNetLog:(id)sender
{
    (void)sender;
    if (colecosession_fujinet_external(_session)) {
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"FujiNet is external"];
        [a setInformativeText:@"Its console output is in the process that "
                              @"is running it."];
        [a runModal];
        return;
    }
    if (_logWindow) {
        [_logWindow makeKeyAndOrderFront:nil];
        return;
    }

    NSScrollView *scroll =
        [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 36, 820, 524)];
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;
    scroll.autohidesScrollers = NO;

    _logView = [[NSTextView alloc] initWithFrame:scroll.bounds];
    _logView.editable = NO;
    _logView.richText = NO;
    _logView.font = [NSFont monospacedSystemFontOfSize:11
                                                weight:NSFontWeightRegular];
    _logView.autoresizingMask = NSViewWidthSizable;
    scroll.documentView = _logView;

    _logWindow = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 820, 560)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _logWindow.title = @"FujiNet Console Log";
    _logWindow.releasedWhenClosed = NO;
    _logWindow.delegate = self;
    {
        /* the log over a strip with the Help button */
        NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 820, 560)];
        NSButton *help = COLECOHelpButton(@"fujilog");
        [help setFrameOrigin:NSMakePoint(10, 6)];
        help.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
        [box addSubview:scroll];
        [box addSubview:help];
        _logWindow.contentView = box;
    }
    COLECOSetHelpContext(_logWindow, @"fujilog");
    [_logWindow center];

    _logTimer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                                 target:self
                                               selector:@selector(refreshLog:)
                                               userInfo:nil
                                                repeats:YES];
    [self refreshLog:nil];
    [_logWindow makeKeyAndOrderFront:nil];
}

- (void)openWebUI:(id)sender
{
    (void)sender;
    if (colecosession_fujinet_external(_session)) {
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"FujiNet is external"];
        [a setInformativeText:@"Use the web UI of the FujiNet instance this "
                              @"app dials."];
        [a runModal];
        return;
    }
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
    /* ROMs first and together, under one report; then the first thing that
     * is not a ROM is opened as before. */
    NSMutableArray<NSString *> *roms = [NSMutableArray array];
    NSString *other = nil;
    for (NSURL *u in urls) {
        NSString *path = [u path];
        if (!path) continue;
        if (colecosession_media_is_rom(path.UTF8String))
            [roms addObject:path];
        else if (!other)
            other = path;
    }
    if (roms.count) [self importRomPaths:roms];
    if (other) [self loadMedia:other];
    return roms.count || other;
}
@end
