/*
 * ColecoControllersWindow -- the AppKit Controllers panel: the ColecoVision's
 * two hand controllers drawn from the session's layout
 * (colecosession_controls_layout) with NSBezierPath, so every frontend shows
 * the same pair; the console strip (Reset Console, Reset to CONFIG); the
 * gamepads and the controller each drives; and Map mode.
 *
 * Each hand controller is a black case with a short stick on a disc (a ring
 * of eight direction wedges round a cap that shifts toward the held
 * direction), a fire button down either side, and the 12-key phone keypad
 * below it on the overlay card's silver frame. One custom-drawn view
 * (ColecoConsoleView) paints it and takes the mouse; the layout, the exact
 * hit shapes and the stick maths are the core's, the colours are here.
 *
 * Gestures: press a key or a fire button and it is held until the button is
 * released or dragged off; press a wedge for that direction; press the stick
 * and drag for eight ways (nothing near the middle). The pointer is its own
 * input source (colecosession_pointer_set), so the panel lets go of
 * everything the mouse holds when it is hidden or loses key status. One
 * pointer holds one control at a time: chords need the keyboard or a pad.
 *
 * What the console sees held -- host keyboard, gamepads and the mouse -- is
 * painted in the accent, so the panel doubles as an input tester for a new
 * gamepad. Keys typed with the panel key go to the console as in the main
 * window; no view here takes the keyboard, so the panel itself is the first
 * responder.
 *
 * FIXED SIZE: an NSPanel without a resize mask, sized from its contents.
 * Hidden at start, ordered out rather than closed, so its position survives.
 * The 30 Hz poll runs only while the panel can be seen.
 *
 * MAP MODE: Map arms a two-step rebind -- click a control on the picture or
 * choose one from the list (all COLECO_TARGET_COUNT targets, the console
 * actions among them), then press a key or a gamepad button; the next one is
 * bound and whatever held it is told so in the status line. Esc cancels the
 * pick; typed keys are not typed while Map is armed. Rebinding steals the key
 * from its old target. F9 and F12 are the app's and are never bound.
 *
 * Written to the family's AppKit pattern; built and verified on CI only (no
 * Apple toolchain here).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "ControllersWindow.h"

#import "../AccentColor.h"
#import "../ColecoKeyForward.h"
#import "../help/HelpWindow.h"

#include <math.h>
#include <string.h>

#define UNIT         34.0    /* points per layout unit (a keypad key's width) */
#define CASE_PAD     16.0    /* the case round the controllers */
#define BRAND_H      20.0    /* the brand strip along the top */
#define TOP_GAP      10.0
#define STRIP_H      28.0    /* the console strip's buttons */
#define STRIP_GAP    10.0
#define MAX_CTL      64      /* room for the core's 50 controls */
#define MARGIN       16.0
#define TITLE_H      18.0
#define ROW_H        26.0
#define EFF_W       108.0
#define POPUP_W     108.0
#define BTN_H        28.0
#define MAX_PAD_ROWS 3
#define SLOW_TICKS   8       /* the 30 Hz tick's 250 ms part */
#define DIM_ALPHA    0.4     /* parts that are not the Map pick */
#define BODY_TOP     0.6f    /* the controller body starts under its caption (layout units) */
#define BODY_BOTTOM  8.65f

/* The skin: colours live here, geometry in the core. */
#define COL_CASE_TOP   0x3A3A3D
#define COL_CASE_BOT   0x1F1F21
#define COL_CASE_EDGE  0x0B0B0C
#define COL_BRAND_BAR  0x151517
#define COL_BRAND_TEXT 0x9496F2
#define COL_BODY_TOP   0x2B2B2E
#define COL_BODY_BOT   0x151517
#define COL_BODY_EDGE  0x050505
#define COL_BODY_LIGHT 0x3C3C40
#define COL_CAPTION    0xE4E0D6
#define COL_PLATE      0x0D0D0E
#define COL_PLATE_RIM  0x18181A
#define COL_WEDGE      0x2A2A2E
#define COL_WEDGE_HOT  0x3A3A40
#define COL_WEDGE_INK  0x9A968C
#define COL_CAP_LIGHT  0x55555B
#define COL_CAP_DARK   0x1C1C1F
#define COL_CAP_RIM    0x0A0A0B
#define COL_FIRE       0xD9461E
#define COL_FIRE_HOT   0xE8602F
#define COL_FIRE_SIDE  0x7C240F
#define COL_CARD_TOP   0xB9BBBE
#define COL_CARD_BOT   0x8E9093
#define COL_CARD_SLOT  0x0B0B0C
#define COL_CARD_LIP   0x4A4A4E
#define COL_KEY        0x1D1D20
#define COL_KEY_HOT    0x2A2A2E
#define COL_KEY_SIDE   0x0A0A0B
#define COL_KEY_INK    0xF2F2F2
#define COL_STRIP      0x3C3C41
#define COL_STRIP_HOT  0x54545B
#define COL_STRIP_SIDE 0x161618

static NSString *nsstr(const char *text)
{
    NSString *res = text ? [NSString stringWithUTF8String:text] : nil;
    return res ? res : @"";
}

static NSString *keyName(uint32_t keysym)
{
    char name[48];
    name[0] = '\0';
    if (keysym) colecosession_keysym_name(keysym, name, (int)sizeof name);
    return nsstr(name);
}

static NSColor *rgb(uint32_t value)
{
    return [NSColor colorWithSRGBRed:((value >> 16) & 0xff) / 255.0
                               green:((value >> 8) & 0xff) / 255.0
                                blue:(value & 0xff) / 255.0
                               alpha:1.0];
}

/* A vertical gradient clipped to a path (view coordinates: the views here are
 * flipped, so "top" is the smaller y). */
static void fillGradient(NSBezierPath *path, uint32_t topRgb, uint32_t botRgb)
{
    const NSRect bnd = [path bounds];
    const CGFloat comps[8] = {
        ((topRgb >> 16) & 0xff) / 255.0, ((topRgb >> 8) & 0xff) / 255.0, (topRgb & 0xff) / 255.0, 1.0,
        ((botRgb >> 16) & 0xff) / 255.0, ((botRgb >> 8) & 0xff) / 255.0, (botRgb & 0xff) / 255.0, 1.0 };
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef grad = CGGradientCreateWithColorComponents(space, comps, NULL, 2);
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    CGContextDrawLinearGradient(ctx, grad, CGPointMake(NSMinX(bnd), NSMinY(bnd)),
                                CGPointMake(NSMinX(bnd), NSMaxY(bnd)), 0);
    [NSGraphicsContext restoreGraphicsState];
    CGGradientRelease(grad);
    CGColorSpaceRelease(space);
}

/* A radial gradient clipped to a path: the light colour at lit, the dark one
 * at the rim of the circle round `centre` with `radius`. */
static void fillRadial(NSBezierPath *path, NSPoint lit, NSPoint centre, CGFloat radius,
                       uint32_t lightRgb, uint32_t darkRgb)
{
    const CGFloat comps[8] = {
        ((lightRgb >> 16) & 0xff) / 255.0, ((lightRgb >> 8) & 0xff) / 255.0, (lightRgb & 0xff) / 255.0, 1.0,
        ((darkRgb >> 16) & 0xff) / 255.0, ((darkRgb >> 8) & 0xff) / 255.0, (darkRgb & 0xff) / 255.0, 1.0 };
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGGradientRef grad = CGGradientCreateWithColorComponents(space, comps, NULL, 2);
    [NSGraphicsContext saveGraphicsState];
    [path addClip];
    CGContextDrawRadialGradient(ctx, grad, CGPointMake(lit.x, lit.y), 0, CGPointMake(centre.x, centre.y),
                                radius, kCGGradientDrawsAfterEndLocation);
    [NSGraphicsContext restoreGraphicsState];
    CGGradientRelease(grad);
    CGColorSpaceRelease(space);
}

/* Lines of text centred in rect, the font shrunk (to 6 pt) until they fit. */
static void drawFitted(NSString *text, NSRect rect, CGFloat size, BOOL bold, NSColor *ink)
{
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    const CGFloat count = (CGFloat)[lines count];
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    para.lineBreakMode = NSLineBreakByClipping;
    NSDictionary *attrs = nil;
    CGFloat lineHeight = 0;
    for (;;) {
        NSFont *font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
        attrs = @{ NSFontAttributeName: font,
                   NSParagraphStyleAttributeName: para,
                   NSForegroundColorAttributeName: ink };
        CGFloat widest = 0;
        lineHeight = 0;
        for (NSString *line in lines) {
            const NSSize extent = [line sizeWithAttributes:attrs];
            if (extent.width > widest) widest = extent.width;
            if (extent.height > lineHeight) lineHeight = extent.height;
        }
        if ((widest <= rect.size.width && lineHeight * count <= rect.size.height + 2) || size <= 6) break;
        size -= 0.5;
    }
    CGFloat ypos = rect.origin.y + floor((rect.size.height - lineHeight * count) / 2);
    for (NSString *line in lines) {
        [line drawInRect:NSMakeRect(rect.origin.x, ypos, rect.size.width, lineHeight) withAttributes:attrs];
        ypos += lineHeight;
    }
}

/* The sector of a ring between radii inner and outer, centred on centreDeg
 * and halfDeg either side of it. Angles are mathematical (counter-clockwise
 * as seen, 0 = right); the view is flipped, so y is subtracted. Straight
 * segments, so nothing depends on how AppKit turns arcs in a flipped view. */
static NSBezierPath *ringSector(NSPoint ctr, CGFloat inner, CGFloat outer, double centreDeg, double halfDeg)
{
    NSBezierPath *path = [NSBezierPath bezierPath];
    const int steps = 8;
    for (int idx = 0; idx <= steps; idx++) {
        const double ang = (centreDeg - halfDeg + 2.0 * halfDeg * idx / steps) * M_PI / 180.0;
        const NSPoint pnt = NSMakePoint(ctr.x + outer * cos(ang), ctr.y - outer * sin(ang));
        if (idx == 0) [path moveToPoint:pnt]; else [path lineToPoint:pnt];
    }
    for (int idx = steps; idx >= 0; idx--) {
        const double ang = (centreDeg - halfDeg + 2.0 * halfDeg * idx / steps) * M_PI / 180.0;
        [path lineToPoint:NSMakePoint(ctr.x + inner * cos(ang), ctr.y - inner * sin(ang))];
    }
    [path closePath];
    return path;
}

/* The direction a wedge points, in degrees, from its action bits. */
static double wedgeAngle(unsigned acts)
{
    const int dirx = (int)((acts >> COLECO_ACT_RIGHT) & 1u) - (int)((acts >> COLECO_ACT_LEFT) & 1u);
    const int diry = (int)((acts >> COLECO_ACT_UP) & 1u) - (int)((acts >> COLECO_ACT_DOWN) & 1u);
    return atan2((double)diry, (double)dirx) * 180.0 / M_PI;
}

static double ease(double cur, double goal)
{
    const double diff = goal - cur;
    return fabs(diff) < 0.004 ? goal : cur + diff * 0.4;
}

/* ---- views --------------------------------------------------------------------- */

/* Laid out from the top, like the other frontends' panels. */
@interface ColecoFlippedView : NSView
@end

@implementation ColecoFlippedView
- (BOOL)isFlipped { return YES; }
@end

/* Keys typed with the panel key go to its owner. */
@class ColecoControllersWindow;
@interface ColecoControllersPanel : NSPanel
@property (nonatomic, weak) ColecoControllersWindow *owner;
@end

@interface ColecoControllersWindow () <NSWindowDelegate>
- (void)mapPick:(int)target;
- (NSString *)legendForTarget:(int)target;
- (void)runStripAction:(int)sysact;
- (void)panelKeyDown:(NSEvent *)evt;
- (void)panelKeyUp:(NSEvent *)evt;
- (void)panelFlagsChanged:(NSEvent *)evt;
@end

/* How far a stick cap is shifted now, eased toward where the held contacts
 * put it: -1..1, +x right, +y toward the player. */
typedef struct { double tiltx, tilty; } ColecoCap;

/* The console: the case with its brand strip, the two hand controllers and the
 * console strip. Everything on it is drawn here and hit-tested here (by the
 * core, except the strip). */
@interface ColecoConsoleView : ColecoFlippedView
@property (nonatomic) colecosession *session;
@property (nonatomic, weak) ColecoControllersWindow *owner;
@property (nonatomic) BOOL mapping;          /* Map armed: bindings shown, nothing lit */
@property (nonatomic) int mapTarget;         /* the control waiting for a key, or -1 */
@property (nonatomic) BOOL showKeys;         /* the bound host key as a small legend */
@property (nonatomic, readonly) NSSize contentSize;
- (void)configure;
/* Reads what the console sees held and moves the caps toward it; redraws when
 * anything changed. */
- (void)poll;
/* Lets go of everything the pointer holds. */
- (void)letGo;
@end

@implementation ColecoConsoleView {
    const coleco_control *_ctl;
    int _num;
    float _layW, _layH;
    NSSize _size;
    BOOL _lit[MAX_CTL];
    unsigned _held[COLECO_PORTS];
    ColecoCap _cap[COLECO_PORTS];
    int _hover;                  /* the control under the pointer, or -1 */
    int _hoverStrip;             /* the console button under the pointer, or -1 */

    /* the mouse */
    int _downIdx;                /* the control pressed, or -1 */
    int _downPort;               /* the controller whose pointer mask is set, or -1 */
    int _downStrip;              /* the console button pressed, or -1 */
    unsigned _downMask;
    BOOL _stickDrag;
    NSTrackingArea *_tracking;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _downIdx = -1;
    _downPort = -1;
    _downStrip = -1;
    _hover = -1;
    _hoverStrip = -1;
    _mapTarget = -1;
    return self;
}

- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)evt { (void)evt; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (NSSize)contentSize { return _size; }

- (void)configure
{
    _ctl = colecosession_controls_layout(&_num, &_layW, &_layH);
    if (!_ctl) _num = 0;
    if (_num > MAX_CTL) _num = MAX_CTL;
    _size = NSMakeSize(ceil(_layW * UNIT) + 2 * CASE_PAD,
                       BRAND_H + TOP_GAP + ceil(_layH * UNIT) + STRIP_GAP + STRIP_H + CASE_PAD);
    memset(_lit, 0, sizeof _lit);
    [self setNeedsDisplay:YES];
}

- (void)updateTrackingAreas
{
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                      NSTrackingActiveAlways | NSTrackingInVisibleRect)
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
    [super updateTrackingAreas];
}

/* ---- geometry ------------------------------------------------------------------ */

- (NSPoint)caseOrigin
{
    return NSMakePoint(CASE_PAD, BRAND_H + TOP_GAP);
}

- (NSRect)rectOf:(const coleco_control *)ctl
{
    const NSPoint org = [self caseOrigin];
    return NSMakeRect(org.x + ctl->xpos * UNIT, org.y + ctl->ypos * UNIT, ctl->width * UNIT, ctl->height * UNIT);
}

/* The two console buttons under the controllers. */
- (NSRect)stripRect:(int)idx
{
    const CGFloat inner = _size.width - 2 * CASE_PAD;
    const CGFloat each = floor((inner - STRIP_GAP) / 2);
    const CGFloat ypos = BRAND_H + TOP_GAP + ceil(_layH * UNIT) + STRIP_GAP;
    return NSMakeRect(CASE_PAD + idx * (each + STRIP_GAP), ypos, each, STRIP_H);
}

- (int)stripAt:(NSPoint)pnt
{
    for (int idx = 0; idx < COLECO_SYSACT_COUNT; idx++)
        if (NSPointInRect(pnt, [self stripRect:idx])) return idx;
    return -1;
}

/* The control under a view point, or -1. */
- (int)hitAt:(NSPoint)pnt
{
    const NSPoint org = [self caseOrigin];
    const int idx = colecosession_controls_hit((float)((pnt.x - org.x) / UNIT), (float)((pnt.y - org.y) / UNIT));
    return idx >= 0 && idx < _num ? idx : -1;
}

/* The STICK of a controller: its centre and its radius, in view points. */
- (BOOL)stickOfPort:(int)port centre:(NSPoint *)centre radius:(CGFloat *)radius
{
    for (int idx = 0; idx < _num; idx++) {
        const coleco_control *ctl = &_ctl[idx];
        if (ctl->kind != COLECO_CTL_STICK || ctl->port != port) continue;
        const NSRect rect = [self rectOf:ctl];
        *centre = NSMakePoint(NSMidX(rect), NSMidY(rect));
        *radius = rect.size.width / 2;
        return YES;
    }
    return NO;
}

/* ---- the held state --------------------------------------------------------------- */

- (void)poll
{
    BOOL dirty = NO;
    const unsigned dirbits = (1u << COLECO_ACT_UP) | (1u << COLECO_ACT_DOWN) |
                             (1u << COLECO_ACT_LEFT) | (1u << COLECO_ACT_RIGHT);
    unsigned stick[COLECO_PORTS];
    for (int port = 0; port < COLECO_PORTS; port++) {
        /* what the console sees, opposed directions already cancelled */
        _held[port] = self.mapping ? 0 : colecosession_buttons_held(_session, port);
        stick[port] = _held[port] & dirbits;
    }

    for (int idx = 0; idx < _num; idx++) {
        const coleco_control *ctl = &_ctl[idx];
        BOOL lit = NO;
        const BOOL known = ctl->port >= 0 && ctl->port < COLECO_PORTS;
        switch (ctl->kind) {
        case COLECO_CTL_KEY:
        case COLECO_CTL_FIRE:
            lit = known && (_held[ctl->port] & ctl->act_mask) != 0;
            break;
        case COLECO_CTL_DIRECTION:
            /* the wedge the stick is in: a diagonal lights only its own */
            lit = known && ctl->act_mask != 0 && stick[ctl->port] == ctl->act_mask;
            break;
        default:
            break;
        }
        if (lit != _lit[idx]) {
            _lit[idx] = lit;
            dirty = YES;
        }
    }

    for (int port = 0; port < COLECO_PORTS; port++) {
        double tiltx = (double)((stick[port] >> COLECO_ACT_RIGHT) & 1u) - (double)((stick[port] >> COLECO_ACT_LEFT) & 1u);
        double tilty = (double)((stick[port] >> COLECO_ACT_DOWN) & 1u) - (double)((stick[port] >> COLECO_ACT_UP) & 1u);
        if (tiltx != 0 && tilty != 0) { tiltx *= 0.7071; tilty *= 0.7071; }
        ColecoCap cap = _cap[port];
        cap.tiltx = ease(cap.tiltx, tiltx);
        cap.tilty = ease(cap.tilty, tilty);
        if (cap.tiltx != _cap[port].tiltx || cap.tilty != _cap[port].tilty) {
            _cap[port] = cap;
            dirty = YES;
        }
    }
    if (dirty) [self setNeedsDisplay:YES];
}

/* ---- drawing -------------------------------------------------------------------------- */

- (NSString *)legendFor:(const coleco_control *)ctl
{
    if (!(self.mapping || self.showKeys) || ctl->target < 0) return nil;
    return [self.owner legendForTarget:ctl->target];
}

- (BOOL)selected:(const coleco_control *)ctl
{
    return self.mapping && ctl->target >= 0 && ctl->target == self.mapTarget;
}

/* Whether a part is drawn faint: in Map mode everything but the pick (or, while
 * nothing is picked yet, the part under the pointer). */
- (BOOL)dimmed:(int)idx
{
    if (!self.mapping) return NO;
    const coleco_control *ctl = &_ctl[idx];
    if (ctl->kind == COLECO_CTL_CAPTION || ctl->kind == COLECO_CTL_OVERLAY) return NO;
    if ([self selected:ctl]) return NO;
    return !(self.mapTarget < 0 && idx == _hover);
}

- (void)beginDim:(BOOL)dim
{
    if (!dim) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextSaveGState(ctx);
    CGContextSetAlpha(ctx, DIM_ALPHA);
    CGContextBeginTransparencyLayer(ctx, NULL);
}

- (void)endDim:(BOOL)dim
{
    if (!dim) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextEndTransparencyLayer(ctx);
    CGContextRestoreGState(ctx);
}

/* A held part: the accent fill with a white outline. */
- (void)strokeHeld:(NSBezierPath *)path
{
    [[NSColor whiteColor] setStroke];
    path.lineWidth = 2;
    [path stroke];
}

/* A hand controller's black case and its name above it. */
- (void)drawBody:(const coleco_control *)ctl
{
    const NSRect cap = [self rectOf:ctl];
    const NSPoint org = [self caseOrigin];
    const NSRect body = NSMakeRect(cap.origin.x, org.y + BODY_TOP * UNIT, cap.size.width,
                                   (BODY_BOTTOM - BODY_TOP) * UNIT);
    NSBezierPath *shape = [NSBezierPath bezierPathWithRoundedRect:body xRadius:14 yRadius:14];
    fillGradient(shape, COL_BODY_TOP, COL_BODY_BOT);
    [rgb(COL_BODY_EDGE) setStroke];
    shape.lineWidth = 1;
    [shape stroke];
    [rgb(COL_BODY_LIGHT) setFill];
    NSRectFill(NSMakeRect(body.origin.x + 14, body.origin.y + 1, body.size.width - 28, 1));
    drawFitted(nsstr(ctl->label), cap, 12, YES, rgb(COL_CAPTION));
}

/* The overlay card's frame behind the keys: a silver faceplate with the card
 * slot along its top edge. Drawn, never pressed. */
- (void)drawCard:(const coleco_control *)ctl
{
    const NSRect rect = [self rectOf:ctl];
    NSBezierPath *panel = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:8 yRadius:8];
    fillGradient(panel, COL_CARD_TOP, COL_CARD_BOT);
    [rgb(COL_CARD_LIP) setStroke];
    panel.lineWidth = 1;
    [panel stroke];
    NSBezierPath *slot = [NSBezierPath bezierPathWithRoundedRect:
        NSMakeRect(rect.origin.x + 10, rect.origin.y + 5, rect.size.width - 20, 3) xRadius:1.5 yRadius:1.5];
    [rgb(COL_CARD_SLOT) setFill];
    [slot fill];
    [rgb(COL_CARD_LIP) setFill];
    NSRectFill(NSMakeRect(rect.origin.x + 10, rect.origin.y + 8, rect.size.width - 20, 1));
}

/* The stick's recessed plate; its cap goes over the wedges (drawCap:). */
- (void)drawPlate:(const coleco_control *)ctl
{
    const NSRect rect = NSInsetRect([self rectOf:ctl], -2, -2);
    NSBezierPath *ring = [NSBezierPath bezierPathWithOvalInRect:rect];
    [rgb(COL_PLATE_RIM) setFill];
    [ring fill];
    NSBezierPath *dish = [NSBezierPath bezierPathWithOvalInRect:NSInsetRect(rect, 2, 2)];
    [rgb(COL_PLATE) setFill];
    [dish fill];
}

/* One of the eight wedges round the cap. */
- (void)drawWedge:(const coleco_control *)ctl idx:(int)idx centre:(NSPoint)ctr radius:(CGFloat)rad lit:(BOOL)lit
{
    const BOOL held = lit || [self selected:ctl];
    const double ang = wedgeAngle(ctl->act_mask);
    NSBezierPath *wedge = ringSector(ctr, rad * 0.5 + 1.5, rad - 1.5, ang, 21.0);
    NSColor *fill = held ? COLECOAccentColor() : rgb(idx == _hover && !self.mapping ? COL_WEDGE_HOT : COL_WEDGE);
    [fill setFill];
    [wedge fill];
    if (held) [self strokeHeld:wedge];
    NSColor *ink = held ? [NSColor whiteColor] : rgb(COL_WEDGE_INK);
    const double rads = ang * M_PI / 180.0;
    const CGFloat mid = rad * 0.75;
    const NSPoint spot = NSMakePoint(ctr.x + mid * cos(rads), ctr.y - mid * sin(rads));
    NSString *key = [self legendFor:ctl];
    if (key.length) {
        drawFitted(key, NSMakeRect(spot.x - 16, spot.y - 6, 32, 12), 8, NO, ink);
        return;
    }
    /* the arrow: a small triangle pointing out */
    const NSPoint out = NSMakePoint(cos(rads), -sin(rads));
    const NSPoint side = NSMakePoint(-out.y, out.x);
    NSBezierPath *arrow = [NSBezierPath bezierPath];
    [arrow moveToPoint:NSMakePoint(spot.x + out.x * 5, spot.y + out.y * 5)];
    [arrow lineToPoint:NSMakePoint(spot.x - out.x * 3 + side.x * 5, spot.y - out.y * 3 + side.y * 5)];
    [arrow lineToPoint:NSMakePoint(spot.x - out.x * 3 - side.x * 5, spot.y - out.y * 3 - side.y * 5)];
    [arrow closePath];
    [ink setFill];
    [arrow fill];
}

/* A side fire button: a tall orange-red button with a dark lower edge. */
- (void)drawFire:(const coleco_control *)ctl idx:(int)idx lit:(BOOL)lit
{
    const BOOL held = lit || [self selected:ctl];
    const NSRect rect = NSInsetRect([self rectOf:ctl], 0.5, 0.5);
    NSBezierPath *side = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:9 yRadius:9];
    [rgb(COL_FIRE_SIDE) setFill];
    [side fill];
    const NSRect topRect = NSMakeRect(rect.origin.x, rect.origin.y, rect.size.width, rect.size.height - 3);
    NSBezierPath *top = [NSBezierPath bezierPathWithRoundedRect:topRect xRadius:9 yRadius:9];
    NSColor *fill = held ? COLECOAccentColor() : rgb(idx == _hover && !self.mapping ? COL_FIRE_HOT : COL_FIRE);
    [fill setFill];
    [top fill];
    if (held) [self strokeHeld:top];
    NSString *key = [self legendFor:ctl];
    if (key.length)
        drawFitted(key, NSInsetRect(topRect, 1, 0), 9, YES, [NSColor whiteColor]);
}

/* A keypad key: black rubber with its legend, the bound key under it. */
- (void)drawKey:(const coleco_control *)ctl idx:(int)idx lit:(BOOL)lit
{
    const BOOL held = lit || [self selected:ctl];
    const NSRect rect = NSInsetRect([self rectOf:ctl], 0.5, 0.5);
    NSBezierPath *side = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:4 yRadius:4];
    [rgb(COL_KEY_SIDE) setFill];
    [side fill];
    const NSRect topRect = NSMakeRect(rect.origin.x, rect.origin.y, rect.size.width, rect.size.height - 3);
    NSBezierPath *top = [NSBezierPath bezierPathWithRoundedRect:topRect xRadius:4 yRadius:4];
    NSColor *fill = held ? COLECOAccentColor() : rgb(idx == _hover && !self.mapping ? COL_KEY_HOT : COL_KEY);
    [fill setFill];
    [top fill];
    if (held) [self strokeHeld:top];
    NSColor *ink = rgb(COL_KEY_INK);
    NSString *key = [self legendFor:ctl];
    const NSRect inner = NSInsetRect(topRect, 2, 1);
    const CGFloat labelH = key.length ? floor(inner.size.height * 0.62) : inner.size.height;
    drawFitted(nsstr(ctl->label), NSMakeRect(inner.origin.x, inner.origin.y, inner.size.width, labelH),
               15, YES, ink);
    if (key.length)
        drawFitted(key, NSMakeRect(inner.origin.x, inner.origin.y + labelH, inner.size.width,
                                   inner.size.height - labelH), 8, NO, rgb(0xc4c4c8));
}

/* The stick's cap: a domed disc that rides toward the held direction. */
- (void)drawCap:(const coleco_control *)ctl dim:(BOOL)dim
{
    const NSRect rect = [self rectOf:ctl];
    const int port = ctl->port >= 0 && ctl->port < COLECO_PORTS ? ctl->port : 0;
    const ColecoCap cap = _cap[port];
    const CGFloat rad = rect.size.width / 2;
    const CGFloat capR = rad * 0.46;
    const NSPoint cen = NSMakePoint(NSMidX(rect) + (CGFloat)cap.tiltx * rad * 0.14,
                                    NSMidY(rect) + (CGFloat)cap.tilty * rad * 0.14);
    [self beginDim:dim];
    /* the shadow lengthens with the tilt */
    NSBezierPath *shadow = [NSBezierPath bezierPathWithOvalInRect:
        NSMakeRect(cen.x - capR + (CGFloat)cap.tiltx * 3 + 1, cen.y - capR + (CGFloat)cap.tilty * 3 + 3,
                   2 * capR, 2 * capR)];
    [[NSColor colorWithSRGBRed:0 green:0 blue:0 alpha:0.5] setFill];
    [shadow fill];
    NSBezierPath *dome = [NSBezierPath bezierPathWithOvalInRect:
        NSMakeRect(cen.x - capR, cen.y - capR, 2 * capR, 2 * capR)];
    fillRadial(dome, NSMakePoint(cen.x - capR * 0.3, cen.y - capR * 0.35), cen, capR,
               COL_CAP_LIGHT, COL_CAP_DARK);
    [rgb(COL_CAP_RIM) setStroke];
    dome.lineWidth = 1.5;
    [dome stroke];
    /* the crescent of light on its upper left */
    NSBezierPath *gleam = [NSBezierPath bezierPath];
    BOOL first = YES;
    for (double deg = 105; deg <= 195; deg += 10) {
        const double rads = deg * M_PI / 180.0;
        const NSPoint pnt = NSMakePoint(cen.x + capR * 0.78 * cos(rads), cen.y - capR * 0.78 * sin(rads));
        if (first) [gleam moveToPoint:pnt]; else [gleam lineToPoint:pnt];
        first = NO;
    }
    gleam.lineWidth = 1.5;
    gleam.lineCapStyle = NSLineCapStyleRound;
    [[NSColor colorWithSRGBRed:1 green:1 blue:1 alpha:0.28] setStroke];
    [gleam stroke];
    [self endDim:dim];
}

/* The brand strip along the top of the case. */
- (void)drawBrand:(NSRect)bnd
{
    [rgb(COL_BRAND_BAR) setFill];
    NSRectFill(NSMakeRect(0, 0, bnd.size.width, BRAND_H));
    [COLECOAccentColor() setFill];
    NSRectFill(NSMakeRect(0, BRAND_H - 2, bnd.size.width, 2));
    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    para.alignment = NSTextAlignmentCenter;
    NSDictionary *attrs = @{ NSFontAttributeName: [NSFont boldSystemFontOfSize:10],
                             NSForegroundColorAttributeName: rgb(COL_BRAND_TEXT),
                             NSParagraphStyleAttributeName: para,
                             NSKernAttributeName: @2.0 };
    [@"COLECOVISION" drawInRect:NSMakeRect(0, 2, bnd.size.width, BRAND_H - 4) withAttributes:attrs];
}

/* The console strip: Reset Console and Reset to CONFIG. */
- (void)drawStrip
{
    for (int idx = 0; idx < COLECO_SYSACT_COUNT; idx++) {
        const int target = COLECO_TARGET_SYSACT(idx);
        const BOOL picked = self.mapping && target == self.mapTarget;
        const BOOL dim = self.mapping && !picked && !(self.mapTarget < 0 && _hoverStrip == idx);
        const NSRect rect = [self stripRect:idx];
        [self beginDim:dim];
        NSBezierPath *side = [NSBezierPath bezierPathWithRoundedRect:rect xRadius:6 yRadius:6];
        [rgb(COL_STRIP_SIDE) setFill];
        [side fill];
        const NSRect topRect = NSMakeRect(rect.origin.x, rect.origin.y, rect.size.width, rect.size.height - 3);
        NSBezierPath *top = [NSBezierPath bezierPathWithRoundedRect:topRect xRadius:6 yRadius:6];
        const BOOL pressed = _downStrip == idx;
        [(picked || pressed ? COLECOAccentColor() : rgb(_hoverStrip == idx && !self.mapping ? COL_STRIP_HOT : COL_STRIP)) setFill];
        [top fill];
        if (picked) [self strokeHeld:top];
        NSString *name = nsstr(coleco_target_short_name(target));
        if (self.mapping || self.showKeys) {
            NSString *key = [self.owner legendForTarget:target];
            if (key.length) name = [NSString stringWithFormat:@"%@  (%@)", name, key];
        }
        drawFitted(name, NSInsetRect(topRect, 4, 0), 12, YES, rgb(COL_CAPTION));
        [self endDim:dim];
    }
}

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    const NSRect bnd = [self bounds];

    /* the case */
    NSBezierPath *frame = [NSBezierPath bezierPathWithRect:bnd];
    fillGradient(frame, COL_CASE_TOP, COL_CASE_BOT);
    [self drawBrand:bnd];
    [rgb(COL_BODY_LIGHT) setFill];
    NSRectFill(NSMakeRect(0, BRAND_H, bnd.size.width, 1));
    NSBezierPath *edge = [NSBezierPath bezierPathWithRect:NSInsetRect(bnd, 0.5, 0.5)];
    [rgb(COL_CASE_EDGE) setStroke];
    edge.lineWidth = 1;
    [edge stroke];

    /* the parts, in the layout's paint order; the caps go over the wedges */
    for (int idx = 0; idx < _num; idx++) {
        const coleco_control *ctl = &_ctl[idx];
        const BOOL dim = [self dimmed:idx];
        [self beginDim:dim];
        switch (ctl->kind) {
        case COLECO_CTL_CAPTION:
            [self drawBody:ctl];
            break;
        case COLECO_CTL_OVERLAY:
            [self drawCard:ctl];
            break;
        case COLECO_CTL_STICK:
            [self drawPlate:ctl];
            break;
        case COLECO_CTL_DIRECTION: {
            NSPoint ctr;
            CGFloat rad;
            if ([self stickOfPort:ctl->port centre:&ctr radius:&rad])
                [self drawWedge:ctl idx:idx centre:ctr radius:rad lit:_lit[idx]];
            break;
        }
        case COLECO_CTL_FIRE:
            [self drawFire:ctl idx:idx lit:_lit[idx]];
            break;
        case COLECO_CTL_KEY:
            [self drawKey:ctl idx:idx lit:_lit[idx]];
            break;
        default:
            break;
        }
        [self endDim:dim];
    }
    for (int idx = 0; idx < _num; idx++)
        if (_ctl[idx].kind == COLECO_CTL_STICK) [self drawCap:&_ctl[idx] dim:self.mapping];

    [self drawStrip];

    /* Map mode: the picked control is called out */
    if (self.mapping && self.mapTarget >= 0) {
        for (int idx = 0; idx < _num; idx++) {
            const coleco_control *ctl = &_ctl[idx];
            if (ctl->target != self.mapTarget || ctl->kind == COLECO_CTL_DIRECTION) continue;
            NSBezierPath *ring = [NSBezierPath bezierPathWithRect:NSInsetRect([self rectOf:ctl], -2, -2)];
            ring.lineWidth = 2;
            [COLECOAccentColor() setStroke];
            [ring stroke];
        }
    }
}

/* ---- the mouse ---------------------------------------------------------------------- */

- (void)setPointerMask:(unsigned)mask
{
    if (_downPort < 0) return;
    _downMask = mask;
    colecosession_pointer_set(_session, _downPort, mask);
}

- (void)letGo
{
    for (int port = 0; port < COLECO_PORTS; port++) colecosession_pointer_set(_session, port, 0);
    _downIdx = -1;
    _downPort = -1;
    _downStrip = -1;
    _downMask = 0;
    _stickDrag = NO;
    [self setNeedsDisplay:YES];
}

- (void)mouseMoved:(NSEvent *)evt
{
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];
    const int hover = [self hitAt:pnt];
    const int strip = [self stripAt:pnt];
    if (hover != _hover || strip != _hoverStrip) {
        _hover = hover;
        _hoverStrip = strip;
        [self setNeedsDisplay:YES];
    }
}

- (void)mouseExited:(NSEvent *)evt
{
    (void)evt;
    if (_hover >= 0 || _hoverStrip >= 0) {
        _hover = -1;
        _hoverStrip = -1;
        [self setNeedsDisplay:YES];
    }
}

- (void)mouseDown:(NSEvent *)evt
{
    if (_downIdx >= 0 || _downStrip >= 0) return;
    const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];

    const int strip = [self stripAt:pnt];
    if (strip >= 0) {
        if (self.mapping) {
            [self.owner mapPick:COLECO_TARGET_SYSACT(strip)];
            return;
        }
        _downStrip = strip;    /* fires on release over the button */
        [self setNeedsDisplay:YES];
        return;
    }

    const int idx = [self hitAt:pnt];
    if (idx < 0) return;
    const coleco_control *ctl = &_ctl[idx];

    if (self.mapping) {
        /* Map: a part with a target of its own is picked; a diagonal drives
         * two contacts and the stick none */
        if (ctl->target >= 0) [self.owner mapPick:ctl->target];
        return;
    }

    switch (ctl->kind) {
    case COLECO_CTL_KEY:
    case COLECO_CTL_FIRE:
        _downIdx = idx;
        _downPort = ctl->port;
        [self setPointerMask:ctl->act_mask];
        break;
    case COLECO_CTL_DIRECTION:
        _downIdx = idx;
        _downPort = ctl->port;
        _stickDrag = YES;
        [self setPointerMask:ctl->act_mask];
        break;
    case COLECO_CTL_STICK:
        /* the cap: nothing in the middle, eight ways out of it */
        _downIdx = idx;
        _downPort = ctl->port;
        _stickDrag = YES;
        [self dragTo:pnt];
        break;
    default:
        break;
    }
    [self poll];
}

- (void)dragTo:(NSPoint)pnt
{
    if (_downStrip >= 0) return;
    if (_downIdx < 0) return;
    if (_stickDrag) {
        NSPoint ctr;
        CGFloat rad;
        if (![self stickOfPort:_downPort centre:&ctr radius:&rad]) return;
        const unsigned mask = colecosession_stick_from_vector((float)(pnt.x - ctr.x), (float)(pnt.y - ctr.y),
                                                              (float)rad);
        if (mask != _downMask) {
            [self setPointerMask:mask];
            [self poll];
        }
        return;
    }
    /* a key or a fire button: dragging off lets go */
    if (!NSPointInRect(pnt, [self rectOf:&_ctl[_downIdx]])) {
        [self letGo];
        [self poll];
    }
}

- (void)mouseDragged:(NSEvent *)evt
{
    [self dragTo:[self convertPoint:[evt locationInWindow] fromView:nil]];
}

- (void)mouseUp:(NSEvent *)evt
{
    if (_downStrip >= 0) {
        /* a console button acts on release over it, like a real one */
        const int strip = _downStrip;
        const NSPoint pnt = [self convertPoint:[evt locationInWindow] fromView:nil];
        _downStrip = -1;
        [self setNeedsDisplay:YES];
        if (NSPointInRect(pnt, [self stripRect:strip])) [self.owner runStripAction:strip];
        return;
    }
    if (_downIdx < 0) return;
    [self letGo];
    [self poll];
}

/* The tooltip: the target's name and what is bound to it. */
- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)pnt userData:(void *)data
{
    (void)view;
    (void)tag;
    (void)data;
    int target = -1;
    const int strip = [self stripAt:pnt];
    if (strip >= 0) {
        target = COLECO_TARGET_SYSACT(strip);
    } else {
        const int idx = [self hitAt:pnt];
        if (idx >= 0) target = _ctl[idx].target;
    }
    if (target < 0) return nil;
    const coleco_binding bind = colecosession_binding_get(_session, target);
    NSString *key = bind.keysym ? keyName(bind.keysym) : @"none";
    NSString *pad = bind.button != COLECO_PAD_BTN_NONE ? nsstr(coleco_pad_button_name(bind.button)) : @"none";
    return [NSString stringWithFormat:@"%@\nKey: %@    Gamepad: %@", nsstr(coleco_target_name(target)), key, pad];
}

/* One tooltip rect over the whole console; the string is made per point. */
- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self removeAllToolTips];
    [self addToolTipRect:[self bounds] owner:self userData:NULL];
}
@end

@implementation ColecoControllersPanel
- (void)keyDown:(NSEvent *)evt { [self.owner panelKeyDown:evt]; }
- (void)keyUp:(NSEvent *)evt { [self.owner panelKeyUp:evt]; }
- (void)flagsChanged:(NSEvent *)evt { [self.owner panelFlagsChanged:evt]; }
/* Escape cancels a Map pick; it is not "close the panel". */
- (void)cancelOperation:(id)sender { (void)sender; }
@end

/* ---- the panel ------------------------------------------------------------------- */

@implementation ColecoControllersWindow {
    colecosession *_session;
    ColecoAppKeyHandler _appKeys;
    ColecoControllersPanel *_panel;
    ColecoFlippedView *_content;
    ColecoConsoleView *_console;
    NSTextField *_padsTitle;
    NSTextField *_noPads;
    NSTextField *_padName[MAX_PAD_ROWS];
    NSTextField *_padEff[MAX_PAD_ROWS];     /* each pad's "drives controller N" */
    NSPopUpButton *_padAssign[MAX_PAD_ROWS];
    int _padRows;
    unsigned _padGeneration;
    NSButton *_mapButton;
    NSButton *_clearButton;
    NSButton *_defaultsButton;
    NSButton *_showKeysBox;
    NSTextField *_pickLabel;
    NSPopUpButton *_pickPopup;              /* every target, for Map */
    NSButton *_helpButton;
    NSTextField *_hint;
    NSTimer *_timer;
    int _slow;
    uint32_t _swallowKey;                   /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a control, >= 0 waiting for a key or
     * gamepad button to bind to that target */
    int _mapState;
    BOOL _placed;
}

- (instancetype)initWithSession:(colecosession *)session appKeys:(ColecoAppKeyHandler)appKeys
{
    self = [super init];
    if (!self) return nil;
    _session = session;
    _appKeys = [appKeys copy];
    _mapState = -2;
    _padGeneration = (unsigned)-1;

    _panel = [[ColecoControllersPanel alloc]
        initWithContentRect:NSMakeRect(0, 0, 420, 640)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskUtilityWindow)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [_panel setTitle:@"Controllers"];
    /* Floats above the machine's window, a full-screen one too. */
    [_panel setLevel:NSFloatingWindowLevel];
    [_panel setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];
    [_panel setReleasedWhenClosed:NO];
    [_panel setBecomesKeyOnlyIfNeeded:NO];
    _panel.owner = self;
    [_panel setDelegate:self];
    COLECOSetHelpContext(_panel, @"controllers");

    _content = [[ColecoFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 420, 640)];
    [_content setAutoresizesSubviews:NO];
    [_panel setContentView:_content];

    _console = [[ColecoConsoleView alloc] initWithFrame:NSZeroRect];
    _console.owner = self;
    _console.session = _session;
    _console.mapTarget = -1;
    {
        /* the older spellings are read when the key is absent */
        int saved = colecosession_get_int(_session, "controllers_show_bindings", -1);
        if (saved < 0) saved = colecosession_get_int(_session, "controllers_bindings", -1);
        if (saved < 0) saved = colecosession_get_int(_session, "controllers_key_names", -1);
        if (saved < 0) saved = colecosession_get_int(_session, "show_bindings", -1);
        _console.showKeys = saved > 0;
    }
    [_content addSubview:_console];

    /* Gamepads: one row per pad, its name, the controller it drives and the
     * one it is set to; rows reserved, so pads coming and going keep the size. */
    _padsTitle = [NSTextField labelWithString:@"Gamepads"];
    [_padsTitle setFont:[NSFont boldSystemFontOfSize:12]];
    [_content addSubview:_padsTitle];
    _noPads = [NSTextField labelWithString:@"No gamepads connected - plug one in"];
    [_noPads setTextColor:[NSColor secondaryLabelColor]];
    [_content addSubview:_noPads];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        _padName[row] = [NSTextField labelWithString:@""];
        [_padName[row] setFont:[NSFont systemFontOfSize:11]];
        [_padName[row] setLineBreakMode:NSLineBreakByTruncatingTail];
        [_content addSubview:_padName[row]];
        _padEff[row] = [NSTextField labelWithString:@""];
        [_padEff[row] setFont:[NSFont systemFontOfSize:11]];
        [_padEff[row] setTextColor:[NSColor secondaryLabelColor]];
        [_padEff[row] setAlignment:NSTextAlignmentRight];
        [_content addSubview:_padEff[row]];
        /* -1 = automatic (connection order), 0 and 1 = that controller */
        _padAssign[row] = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        [_padAssign[row] addItemsWithTitles:@[@"Automatic", @"Controller 1", @"Controller 2"]];
        _padAssign[row].tag = row;
        _padAssign[row].target = self;
        _padAssign[row].action = @selector(padAssignChanged:);
        [_padAssign[row] setRefusesFirstResponder:YES];
        [_content addSubview:_padAssign[row]];
    }

    _mapButton = [NSButton buttonWithTitle:@"Map" target:self action:@selector(toggleMap:)];
    [_mapButton setRefusesFirstResponder:YES];
    [_content addSubview:_mapButton];
    _clearButton = [NSButton buttonWithTitle:@"Clear" target:self action:@selector(clearBinding:)];
    [_clearButton setRefusesFirstResponder:YES];
    [_content addSubview:_clearButton];
    _defaultsButton = [NSButton buttonWithTitle:@"Defaults" target:self action:@selector(restoreDefaults:)];
    [_defaultsButton setRefusesFirstResponder:YES];
    [_content addSubview:_defaultsButton];
    _showKeysBox = [NSButton checkboxWithTitle:@"Show bindings" target:self action:@selector(toggleShowKeys:)];
    [_showKeysBox setState:_console.showKeys ? NSControlStateValueOn : NSControlStateValueOff];
    [_showKeysBox setRefusesFirstResponder:YES];
    [_content addSubview:_showKeysBox];
    _pickLabel = [NSTextField labelWithString:@"Control"];
    [_content addSubview:_pickLabel];
    _pickPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (int target = 0; target < COLECO_TARGET_COUNT; target++)
        [[_pickPopup menu] addItemWithTitle:nsstr(coleco_target_name(target)) action:nil keyEquivalent:@""];
    _pickPopup.target = self;
    _pickPopup.action = @selector(pickChanged:);
    [_pickPopup setRefusesFirstResponder:YES];
    [_content addSubview:_pickPopup];
    _helpButton = COLECOHelpButton(@"controllers");
    [_helpButton setRefusesFirstResponder:YES];
    [_content addSubview:_helpButton];
    _hint = [NSTextField wrappingLabelWithString:@""];
    [_hint setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [_hint setTextColor:[NSColor secondaryLabelColor]];
    [_hint setMaximumNumberOfLines:2];
    [[_hint cell] setTruncatesLastVisibleLine:YES];
    [_content addSubview:_hint];

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(occlusionChanged:)
                                                 name:NSWindowDidChangeOcclusionStateNotification
                                               object:_panel];

    [_console configure];
    [self setMapState:-2];
    [self relayout];
    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_timer invalidate];
    [_panel setDelegate:nil];
}

/* ---- the console ------------------------------------------------------------------- */

/* A new content size with the top-left corner where it was. */
- (void)fitPanel:(NSSize)size
{
    const NSRect frame = [_panel frame];
    NSRect content = [_panel contentRectForFrameRect:frame];
    if (NSEqualSizes(content.size, size)) return;
    content.size = size;
    NSRect next = [_panel frameRectForContentRect:content];
    next.origin.x = frame.origin.x;
    next.origin.y = NSMaxY(frame) - next.size.height;
    [_panel setFrame:next display:YES];
}

- (void)relayout
{
    const NSSize csize = _console.contentSize;
    const CGFloat wide = csize.width;
    const CGFloat inner = wide - 2 * MARGIN;
    const CGFloat padsY = csize.height + 10;
    const CGFloat rowsY = padsY + TITLE_H;
    const CGFloat mapY = rowsY + MAX_PAD_ROWS * ROW_H + 6;
    const CGFloat pickY = mapY + BTN_H + 6;
    const CGFloat hintY = pickY + 26 + 4;
    const CGFloat total = hintY + 34 + MARGIN;

    [self fitPanel:NSMakeSize(wide, total)];
    [_console setFrame:NSMakeRect(0, 0, csize.width, csize.height)];
    [_padsTitle setFrame:NSMakeRect(MARGIN, padsY, inner, 16)];
    [_noPads setFrame:NSMakeRect(MARGIN, rowsY + 4, inner, 18)];
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const CGFloat ypos = rowsY + row * ROW_H;
        [_padName[row] setFrame:NSMakeRect(MARGIN, ypos + 5, inner - POPUP_W - EFF_W - 12, 16)];
        [_padEff[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W - EFF_W - 6, ypos + 5, EFF_W, 16)];
        [_padAssign[row] setFrame:NSMakeRect(MARGIN + inner - POPUP_W, ypos, POPUP_W, 25)];
    }
    [_mapButton setFrame:NSMakeRect(MARGIN, mapY, 56, BTN_H)];
    [_clearButton setFrame:NSMakeRect(MARGIN + 62, mapY, 56, BTN_H)];
    [_defaultsButton setFrame:NSMakeRect(MARGIN + 124, mapY, 72, BTN_H)];
    [_showKeysBox setFrame:NSMakeRect(MARGIN + 206, mapY + 3, 122, 22)];
    [_helpButton setFrameOrigin:NSMakePoint(wide - MARGIN - _helpButton.frame.size.width, mapY + 2)];
    [_pickLabel setFrame:NSMakeRect(MARGIN, pickY + 4, 50, 18)];
    [_pickPopup setFrame:NSMakeRect(MARGIN + 54, pickY, inner - 54, 25)];
    [_hint setFrame:NSMakeRect(MARGIN, hintY, inner, 34)];
    [_console setNeedsDisplay:YES];
}

/* Lets go of what the mouse holds in this panel. */
- (void)letGo
{
    [_console letGo];
}

/* ---- Map mode --------------------------------------------------------------------- */

- (NSString *)legendForTarget:(int)target
{
    return keyName(colecosession_binding_get(_session, target).keysym);
}

/* What is bound to a target, for the status line. */
- (NSString *)bindingText:(int)target
{
    const coleco_binding bind = colecosession_binding_get(_session, target);
    NSString *key = keyName(bind.keysym);
    NSString *text = key.length ? key : @"no key";
    if (bind.button != COLECO_PAD_BTN_NONE)
        text = [NSString stringWithFormat:@"%@, pad %@", text, nsstr(coleco_pad_button_name(bind.button))];
    return text;
}

/* In Map mode nothing is lit and every control shows what is bound to it. */
- (void)refreshLabels
{
    _console.mapping = _mapState != -2;
    _console.mapTarget = _mapState >= 0 ? _mapState : -1;
    [_console setNeedsDisplay:YES];
    [_console poll];
}

- (void)setMapState:(int)state
{
    _mapState = state;
    if (state >= 0) {
        [self letGo];
        colecosession_gamepad_capture_begin(_session);
        _hint.stringValue = [NSString stringWithFormat:@"Press a key or a gamepad button for %@ (now: %@). Esc cancels.",
                             nsstr(coleco_target_name(state)), [self bindingText:state]];
    } else {
        colecosession_gamepad_capture_cancel(_session);
        _hint.stringValue = state == -1
            ? @"Click a control, or choose one from the list, to remap it."
            : @"Click and hold the keys and fire buttons, or drag the stick (eight ways). Keys and "
              @"gamepads work here too.";
    }
    [_mapButton setTitle:(state == -2 ? @"Map" : @"Done")];
    COLECOSetButtonAccent(_mapButton, state != -2);
    _clearButton.enabled = state >= 0;
    _pickPopup.enabled = state != -2;
    if (state >= 0 && state < [_pickPopup numberOfItems]) [_pickPopup selectItemAtIndex:state];
    [self refreshLabels];
}

/* A control clicked on the picture in Map mode. */
- (void)mapPick:(int)target
{
    if (_mapState != -2) [self setMapState:target];
}

- (void)pickChanged:(NSPopUpButton *)sender
{
    if (_mapState == -2) return;
    const NSInteger sel = sender.indexOfSelectedItem;
    if (sel >= 0 && sel < COLECO_TARGET_COUNT) [self setMapState:(int)sel];
}

- (void)showBound:(NSString *)name target:(int)target stolen:(const char *)stolen
{
    NSString *what = nsstr(coleco_target_name(target));
    _hint.stringValue = stolen[0]
        ? [NSString stringWithFormat:@"Bound %@ to %@ (was %@)", name, what, nsstr(stolen)]
        : [NSString stringWithFormat:@"Bound %@ to %@", name, what];
}

- (void)bindKey:(uint32_t)keysym swallow:(BOOL)swallow
{
    char stolen[256];
    stolen[0] = '\0';
    const int target = _mapState;
    colecosession_binding_set_key(_session, target, keysym, stolen, (int)sizeof stolen);
    if (swallow) _swallowKey = keysym;
    [self setMapState:-1];   /* stay armed: remapping several in a row is normal */
    [self showBound:keyName(keysym) target:target stolen:stolen];
}

- (void)pollCapture
{
    int button = COLECO_PAD_BTN_NONE;
    char stolen[256];
    if (_mapState < 0 || !colecosession_gamepad_capture_poll(_session, &button)) return;
    stolen[0] = '\0';
    const int target = _mapState;
    colecosession_binding_set_button(_session, target, button, stolen, (int)sizeof stolen);
    [self setMapState:-1];
    [self showBound:nsstr(coleco_pad_button_name(button)) target:target stolen:stolen];
}

- (void)toggleMap:(id)sender
{
    (void)sender;
    [self setMapState:(_mapState == -2 ? -1 : -2)];
}

/* Clear: the picked control loses its key and its gamepad button. */
- (void)clearBinding:(id)sender
{
    (void)sender;
    if (_mapState < 0) return;
    const int target = _mapState;
    colecosession_binding_set_key(_session, target, 0, NULL, 0);
    colecosession_binding_set_button(_session, target, COLECO_PAD_BTN_NONE, NULL, 0);
    [self setMapState:-1];
    _hint.stringValue = [NSString stringWithFormat:@"Cleared %@", nsstr(coleco_target_name(target))];
}

- (void)restoreDefaults:(id)sender
{
    (void)sender;
    colecosession_bindings_reset(_session);
    [self refreshLabels];
    _hint.stringValue = @"Default bindings restored";
}

- (void)toggleShowKeys:(id)sender
{
    (void)sender;
    const BOOL show = [_showKeysBox state] == NSControlStateValueOn;
    colecosession_set_int(_session, "controllers_show_bindings", show ? 1 : 0);
    _console.showKeys = show;
    [_console setNeedsDisplay:YES];
}

/* A console button on the strip was released over itself. */
- (void)runStripAction:(int)sysact
{
    if (self.sysactionHandler) self.sysactionHandler(sysact);
    else colecosession_sysaction(_session, sysact);
}

/* ---- gamepads ----------------------------------------------------------------------- */

- (void)refreshPadPorts
{
    for (int row = 0; row < _padRows; row++) {
        const int port = colecosession_gamepad_effective_port(_session, row);
        NSString *text = port >= 0 ? [NSString stringWithFormat:@"drives controller %d", port + 1]
                                   : @"drives nothing";
        if (![_padEff[row].stringValue isEqualToString:text]) _padEff[row].stringValue = text;
    }
}

- (void)refreshPads
{
    const int count = colecosession_gamepad_count(_session);
    _padGeneration = colecosession_gamepad_generation(_session);
    _padRows = count < MAX_PAD_ROWS ? (count > 0 ? count : 0) : MAX_PAD_ROWS;
    _noPads.hidden = count > 0;
    _padsTitle.stringValue = count > MAX_PAD_ROWS
        ? [NSString stringWithFormat:@"Gamepads (%d of %d shown)", MAX_PAD_ROWS, count]
        : @"Gamepads";
    for (int row = 0; row < MAX_PAD_ROWS; row++) {
        const BOOL shown = row < _padRows;
        _padName[row].hidden = !shown;
        _padEff[row].hidden = !shown;
        _padAssign[row].hidden = !shown;
        if (!shown) continue;
        char name[128];
        name[0] = '\0';
        colecosession_gamepad_name(_session, row, name, (int)sizeof name);
        _padName[row].stringValue = nsstr(name);
        _padName[row].toolTip = nsstr(name);
        const int assign = colecosession_gamepad_assignment(_session, row);
        [_padAssign[row] selectItemAtIndex:(assign < 0 || assign >= COLECO_PORTS ? 0 : assign + 1)];
    }
    [self refreshPadPorts];
}

- (void)padAssignChanged:(NSPopUpButton *)sender
{
    const int sel = (int)sender.indexOfSelectedItem;
    colecosession_gamepad_assign(_session, (int)sender.tag, sel <= 0 ? -1 : sel - 1);
    [self refreshPadPorts];
}

/* ---- the poll ----------------------------------------------------------------------- */

/* 30 Hz while the panel can be seen: what is held and the gamepad capture;
 * every 250 ms the hot-plugged pads and the auto-assignment. */
- (void)tick
{
    if (_mapState >= 0) [self pollCapture];
    if (++_slow >= SLOW_TICKS) {
        _slow = 0;
        if (colecosession_gamepad_generation(_session) != _padGeneration) [self refreshPads];
        else [self refreshPadPorts];
    }
    [_console poll];
}

/* The timer runs only while the panel is on screen and not occluded or
 * hidden with its app. */
- (void)updateTimer
{
    const BOOL want = [_panel isVisible] && ([_panel occlusionState] & NSWindowOcclusionStateVisible);
    if (want && !_timer) {
        __weak ColecoControllersWindow *weakSelf = self;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *tmr) {
            (void)tmr;
            [weakSelf tick];
        }];
        _timer.tolerance = 0.005;
    } else if (!want && _timer) {
        [_timer invalidate];
        _timer = nil;
    }
}

- (void)occlusionChanged:(NSNotification *)note
{
    (void)note;
    [self updateTimer];
}

/* ---- the keyboard ------------------------------------------------------------------- */

- (void)panelKeyDown:(NSEvent *)evt
{
    /* Command shortcuts are the menus'; AppKit sends no keyUp while Command
     * is down, so forwarding one would leave the machine holding the key. */
    if ([evt modifierFlags] & NSEventModifierFlagCommand) return;
    const uint32_t keysym = ColecoKeysymFromEvent(evt);
    if (!keysym) return;
    if (_swallowKey && keysym == _swallowKey) return;
    if ([evt isARepeat]) return;
    if (keysym == COLECOSESSION_KEYSYM_F9 || keysym == COLECOSESSION_KEYSYM_F12) {
        if (_appKeys) _appKeys(keysym);
        return;
    }
    if (_mapState >= 0) {
        /* the next key is the binding; Esc backs out of the pick */
        if (keysym == COLECOSESSION_KEYSYM_ESCAPE) [self setMapState:-1];
        else [self bindKey:keysym swallow:YES];
        return;
    }
    if (_mapState == -1) return;    /* armed: typing does nothing */
    /* a key bound to a console action is the app's, on its leading edge */
    if (_appKeys && _appKeys(keysym)) return;
    colecosession_key(_session, keysym, 1);
}

- (void)panelKeyUp:(NSEvent *)evt
{
    const uint32_t keysym = ColecoKeysymFromEvent(evt);
    if (!keysym) return;
    if (_swallowKey && keysym == _swallowKey) {
        _swallowKey = 0;
        return;
    }
    colecosession_key(_session, keysym, 0);
}

/* Shift, Control and Option bind too (Command is the menus'); otherwise they
 * are the console's (Left Ctrl and Left Alt are the fire buttons). A release
 * always goes through. */
- (void)panelFlagsChanged:(NSEvent *)evt
{
    int down = 0;
    const uint32_t keysym = ColecoKeysymFromFlagsChange(evt, &down);
    if (!keysym) return;
    if (_mapState >= 0 && down) {
        [self bindKey:keysym swallow:NO];
        return;
    }
    if (_mapState != -2 && down) return;
    colecosession_key(_session, keysym, down);
}

/* ---- the window --------------------------------------------------------------------- */

/* Hidden: Map disarmed, nothing held, no polling. */
- (void)quiesce
{
    [self setMapState:-2];
    [self letGo];
    colecosession_release_all(_session);
    _swallowKey = 0;
    [_timer invalidate];
    _timer = nil;
}

- (void)show
{
    [self refreshPads];
    [self setMapState:-2];
    if (!_placed) {
        [_panel center];
        _placed = YES;
    }
    [_panel makeFirstResponder:nil];    /* the panel itself takes the keys */
    [_panel makeKeyAndOrderFront:nil];
    [self updateTimer];
}

- (void)hide
{
    [_panel orderOut:nil];
    [self quiesce];
}

- (void)toggle
{
    if ([self isVisible]) [self hide];
    else [self show];
}

- (BOOL)isVisible
{
    return [_panel isVisible];
}

- (void)refresh
{
    [self refreshPads];
    [self refreshLabels];
}

/* The panel's mouse and keyboard holds: the main window releases its own. */
- (void)windowDidResignKey:(NSNotification *)note
{
    (void)note;
    [self letGo];
    colecosession_release_all(_session);
    _swallowKey = 0;    /* its release goes elsewhere */
}

- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [self quiesce];
}
@end
