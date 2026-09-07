/*
 * ColecoKeypadWindow -- the AppKit controller panel.
 *
 * Buttons are a PadButton subclass that reports mouseDown and mouseUp rather
 * than an action on click, for the reason every frontend in this family
 * shares: the emulator samples the controller register once per frame, so a
 * value present only for the instant of a click falls between frames and a
 * polling game -- which is how essentially every ColecoVision title reads its
 * skill select -- never sees it. A button here is HELD.
 *
 * mouseUp arrives even when the pointer has left the button (AppKit tracks
 * the drag for the view that got mouseDown), so dragging off cannot strand
 * the machine with a key held forever.
 *
 * Singleton, ordered out rather than closed, so a remap survives closing it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "KeypadWindow.h"

#import "../ColecoKeyForward.h"

/* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key. */
static int g_mapState = -2;
static ColecoKeypadWindow *g_singleton;
static colecosession *g_session;
static coleco_input_state g_keys;

/* NOT named `target`: NSControl already has a `target` property -- the object
 * an action is sent to -- and shadowing it with an int silently breaks the
 * control's own machinery. `padTarget` is the binding-table index. */
@interface PadButton : NSButton
@property (nonatomic) int padTarget;
@property (nonatomic, copy) NSString *face;
@end

@implementation PadButton
- (void)mouseDown:(NSEvent *)e
{
    (void)e;
    if (g_mapState == -1) {
        g_mapState = self.padTarget;
        [[NSNotificationCenter defaultCenter] postNotificationName:@"ColecoPadRefresh"
                                                            object:nil];
        return;
    }
    if (g_mapState >= 0) return;
    if (self.padTarget >= COLECO_TARGET_SYSACT(0)) return;  /* fires on release */
    colecosession_press(g_session, self.padTarget / COLECO_ACT_PER_PORT,
                        self.padTarget % COLECO_ACT_PER_PORT, 1);
    [self highlight:YES];
}

- (void)mouseUp:(NSEvent *)e
{
    (void)e;
    [self highlight:NO];
    if (g_mapState != -2) return;
    if (self.padTarget >= COLECO_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off must not reset the console. */
        colecosession_sysaction(g_session,
                                self.padTarget - COLECO_TARGET_SYSACT(0));
        return;
    }
    colecosession_press(g_session, self.padTarget / COLECO_ACT_PER_PORT,
                        self.padTarget % COLECO_ACT_PER_PORT, 0);
}
@end

@implementation ColecoKeypadWindow {
    NSMutableArray<PadButton *> *_buttons;
    NSButton *_mapButton;
    NSTextField *_hint;
}

static NSString *const kFace[COLECO_KEYPAD_KEYS] = {
    @"0", @"1", @"2", @"3", @"4", @"5", @"6", @"7", @"8", @"9", @"*", @"#"
};
/* The physical 3x4 layout: 1-9 in reading order, then * 0 #. */
static const int kOrder[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11 };

#define BTN_W 46.0
#define BTN_H 30.0
#define GAP    6.0

- (PadButton *)buttonWithFace:(NSString *)face target:(int)target
                        frame:(NSRect)frame
{
    PadButton *b = [[PadButton alloc] initWithFrame:frame];
    [b setTitle:face];
    [b setBezelStyle:NSBezelStyleRounded];
    b.padTarget = target;
    b.face = face;
    /* Not focusable: clicking a pad button must not steal the key window's
     * first responder, and tabbing through thirty-eight buttons is nobody's
     * idea of input. */
    [b setRefusesFirstResponder:YES];
    [_buttons addObject:b];
    return b;
}

- (CGFloat)buildController:(int)port intoView:(NSView *)parent
                         x:(CGFloat)x0 topY:(CGFloat)topY
{
    const CGFloat colw = BTN_W + GAP;
    CGFloat y = topY;

    NSTextField *title = [NSTextField labelWithString:
        [NSString stringWithFormat:@"Controller %d", port + 1]];
    [title setAlignment:NSTextAlignmentCenter];
    [title setFont:[NSFont boldSystemFontOfSize:12]];
    [title setFrame:NSMakeRect(x0, y - 18, 3 * colw - GAP, 16)];
    [parent addSubview:title];
    y -= 24;

    for (int i = 0; i < 12; i++) {
        const int key = kOrder[i];
        NSRect r = NSMakeRect(x0 + (i % 3) * colw,
                              y - (i / 3 + 1) * (BTN_H + GAP),
                              BTN_W, BTN_H);
        [parent addSubview:[self buttonWithFace:kFace[key]
                                         target:COLECO_TARGET_PORT(port, COLECO_ACT_KEYPAD + key)
                                          frame:r]];
    }
    y -= 4 * (BTN_H + GAP) + GAP;

    [parent addSubview:[self buttonWithFace:@"▲"
        target:COLECO_TARGET_PORT(port, COLECO_ACT_UP)
         frame:NSMakeRect(x0 + colw, y - BTN_H, BTN_W, BTN_H)]];
    [parent addSubview:[self buttonWithFace:@"◀"
        target:COLECO_TARGET_PORT(port, COLECO_ACT_LEFT)
         frame:NSMakeRect(x0, y - 2 * BTN_H - GAP, BTN_W, BTN_H)]];
    [parent addSubview:[self buttonWithFace:@"▶"
        target:COLECO_TARGET_PORT(port, COLECO_ACT_RIGHT)
         frame:NSMakeRect(x0 + 2 * colw, y - 2 * BTN_H - GAP, BTN_W, BTN_H)]];
    [parent addSubview:[self buttonWithFace:@"▼"
        target:COLECO_TARGET_PORT(port, COLECO_ACT_DOWN)
         frame:NSMakeRect(x0 + colw, y - 3 * BTN_H - 2 * GAP, BTN_W, BTN_H)]];
    y -= 3 * (BTN_H + GAP) + GAP;

    [parent addSubview:[self buttonWithFace:@"Fire L"
        target:COLECO_TARGET_PORT(port, COLECO_ACT_FIRE_L)
         frame:NSMakeRect(x0, y - BTN_H, BTN_W + colw / 2, BTN_H)]];
    [parent addSubview:[self buttonWithFace:@"Fire R"
        target:COLECO_TARGET_PORT(port, COLECO_ACT_FIRE_R)
         frame:NSMakeRect(x0 + colw + colw / 2, y - BTN_H,
                          BTN_W + colw / 2, BTN_H)]];
    return y - BTN_H - GAP;
}

- (instancetype)init
{
    const CGFloat colw = BTN_W + GAP;
    const CGFloat panelw = 3 * colw - GAP;
    const CGFloat width = GAP + panelw + GAP * 3 + panelw + GAP;
    const CGFloat height = 330;

    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, width, height)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [win setTitle:@"Controllers"];
    /* A utility panel: floats above the machine's window and stays out of the
     * way, which is what this is for. */
    [win setLevel:NSFloatingWindowLevel];
    [win setReleasedWhenClosed:NO];

    self = [super initWithWindow:win];
    if (!self) return nil;

    _buttons = [NSMutableArray array];
    coleco_input_reset(&g_keys);

    NSView *content = [win contentView];
    CGFloat y = height - GAP;
    [self buildController:0 intoView:content x:GAP topY:y];
    CGFloat bottom = [self buildController:1 intoView:content
                                         x:GAP + panelw + GAP * 3 topY:y];

    bottom -= GAP;
    [content addSubview:[self buttonWithFace:@"Reset Console"
        target:COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET)
         frame:NSMakeRect(GAP, bottom - BTN_H, panelw, BTN_H)]];
    [content addSubview:[self buttonWithFace:@"Reset to CONFIG"
        target:COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET_CONFIG)
         frame:NSMakeRect(GAP + panelw + GAP * 3, bottom - BTN_H, panelw, BTN_H)]];
    bottom -= BTN_H + GAP;

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self
                                    action:@selector(toggleMap:)];
    [_mapButton setFrame:NSMakeRect(GAP, bottom - BTN_H, 70, BTN_H)];
    [content addSubview:_mapButton];

    NSButton *defaults = [NSButton buttonWithTitle:@"Defaults" target:self
                                            action:@selector(restoreDefaults:)];
    [defaults setFrame:NSMakeRect(GAP + 76, bottom - BTN_H, 90, BTN_H)];
    [content addSubview:defaults];

    _hint = [NSTextField labelWithString:@""];
    [_hint setFrame:NSMakeRect(GAP + 176, bottom - BTN_H + 6, width - 190, 18)];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [content addSubview:_hint];

    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(refresh)
               name:@"ColecoPadRefresh" object:nil];
    [self refresh];
    return self;
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    g_mapState = (g_mapState == -2) ? -1 : -2;
    [self refresh];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    coleco_bindings_reset_defaults(g_session);
    [self refresh];
}

- (void)refresh
{
    [_mapButton setTitle:(g_mapState == -2 ? @"Map" : @"Cancel")];
    if (g_mapState == -2)
        [_hint setStringValue:@""];
    else if (g_mapState == -1)
        [_hint setStringValue:@"Click a control to remap"];
    else
        [_hint setStringValue:[NSString stringWithFormat:@"Press a key for %s",
                               coleco_binding_label(g_mapState)]];

    for (PadButton *b in _buttons) {
        if (g_mapState != -2) {
            const char *k = coleco_binding_key_name(b.padTarget);
            [b setTitle:(*k ? [NSString stringWithUTF8String:k] : @"—")];
        } else {
            [b setTitle:b.face];
        }
    }
}

/* Keyboard here behaves exactly as in the main window, so typing drives the
 * machine whichever window is key. */
- (void)keyDown:(NSEvent *)e
{
    const uint32_t ks = ColecoKeysymFromEvent(e);
    if (g_mapState >= 0) {
        if (ks) {
            coleco_binding_set(g_session, g_mapState, ks);
            g_mapState = -1;   /* stay armed: remapping several in a row is
                                * the normal case */
            [self refresh];
        }
        return;
    }
    if (g_mapState == -1 || !ks) return;

    const int sa = coleco_input_key_sysaction(ks);
    if (sa >= 0) { colecosession_sysaction(g_session, sa); return; }
    if (coleco_input_key(&g_keys, ks, 1)) {
        colecosession_joystick_raw(g_session, 0, coleco_input_word(&g_keys, 0));
        colecosession_joystick_raw(g_session, 1, coleco_input_word(&g_keys, 1));
    }
}

- (void)keyUp:(NSEvent *)e
{
    if (g_mapState != -2) return;
    const uint32_t ks = ColecoKeysymFromEvent(e);
    if (ks && coleco_input_key(&g_keys, ks, 0)) {
        colecosession_joystick_raw(g_session, 0, coleco_input_word(&g_keys, 0));
        colecosession_joystick_raw(g_session, 1, coleco_input_word(&g_keys, 1));
    }
}

+ (void)toggleWithSession:(colecosession *)session
{
    g_session = session;
    if (!g_singleton) g_singleton = [[ColecoKeypadWindow alloc] init];

    if ([[g_singleton window] isVisible]) {
        g_mapState = -2;
        /* Order out, do not close: a remap in progress and the window's
         * position both survive. */
        [[g_singleton window] orderOut:nil];
    } else {
        [[g_singleton window] makeKeyAndOrderFront:nil];
    }
}

+ (BOOL)isVisible
{
    return g_singleton && [[g_singleton window] isVisible];
}
@end
