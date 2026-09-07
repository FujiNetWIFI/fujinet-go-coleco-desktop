/*
 * ColecoDisplayView -- the framebuffer, on a CVDisplayLink.
 *
 * CVDisplayLink is this platform's frame clock -- the equivalent of
 * GdkFrameClock and DwmFlush -- and feeding it to the session is what lets
 * the emulator phase-lock to the panel instead of beating against it.
 *
 * Its callback runs on its OWN high-priority thread, not the main one. So it
 * does the two cheap things (hand over the tick, pull the frame) and then
 * asks AppKit to redraw on the main thread. Drawing from the callback thread
 * would be a use of AppKit off the main thread, which is undefined.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "DisplayView.h"

#import <CoreVideo/CoreVideo.h>

/* mach_timebase_info and its data type. CoreVideo hands out host time in
 * mach_absolute_time units, which are NOT nanoseconds on every machine --
 * the ratio is what this converts by. */
#include <mach/mach_time.h>

#include <stdlib.h>
#include <string.h>

@implementation ColecoDisplayView {
    colecosession *_session;
    CVDisplayLinkRef _link;
    uint16_t *_fb;
    uint32_t *_rgb;
    uint64_t _serial;
    CGContextRef _ctx;
    CGColorSpaceRef _cs;
    BOOL _tv;
    BOOL _smooth;
}

static CVReturn displayCallback(CVDisplayLinkRef link,
                                const CVTimeStamp *now,
                                const CVTimeStamp *out,
                                CVOptionFlags flagsIn, CVOptionFlags *flagsOut,
                                void *ctx)
{
    (void)link; (void)out; (void)flagsIn; (void)flagsOut;
    ColecoDisplayView *self = (__bridge ColecoDisplayView *)ctx;
    [self tick:now->hostTime];
    return kCVReturnSuccess;
}

- (instancetype)initWithSession:(colecosession *)session
{
    self = [super initWithFrame:NSMakeRect(0, 0, 848, 636)];
    if (!self) return nil;
    _session = session;
    _tv = YES;
    _smooth = NO;

    const size_t n = COLECOSESSION_FB_WIDTH * COLECOSESSION_FB_HEIGHT;
    _fb = calloc(n, sizeof *_fb);
    _rgb = calloc(n, sizeof *_rgb);
    _cs = CGColorSpaceCreateDeviceRGB();
    _ctx = CGBitmapContextCreate(_rgb, COLECOSESSION_FB_WIDTH,
                                 COLECOSESSION_FB_HEIGHT, 8,
                                 COLECOSESSION_FB_WIDTH * 4, _cs,
                                 kCGImageAlphaNoneSkipFirst |
                                 kCGBitmapByteOrder32Little);

    CVDisplayLinkCreateWithActiveCGDisplays(&_link);
    CVDisplayLinkSetOutputCallback(_link, displayCallback,
                                   (__bridge void *)self);
    CVDisplayLinkStart(_link);
    return self;
}

- (void)stop
{
    if (_link) {
        CVDisplayLinkStop(_link);
        CVDisplayLinkRelease(_link);
        _link = NULL;
    }
}

- (void)dealloc
{
    [self stop];
    if (_ctx) CGContextRelease(_ctx);
    if (_cs) CGColorSpaceRelease(_cs);
    free(_fb);
    free(_rgb);
}

- (void)tick:(uint64_t)hostTime
{
    /* mach_absolute_time units, converted once. */
    static double toNs = 0.0;
    if (toNs == 0.0) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        toNs = (double)tb.numer / (double)tb.denom;
    }
    colecosession_notify_vsync(_session, (int64_t)((double)hostTime * toNs));

    if (!colecosession_copy_frame(_session, _fb, &_serial))
        return;

    const int n = COLECOSESSION_FB_WIDTH * COLECOSESSION_FB_HEIGHT;
    for (int i = 0; i < n; i++) {
        const uint16_t p = _fb[i];
        unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
        /* Replicate the high bits rather than shifting left: a plain shift
         * maps full-scale 0x1F to 0xF8, so white comes out slightly grey. */
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        _rgb[i] = (r << 16) | (g << 8) | b;
    }
    /* AppKit is main-thread only; the display link's callback is not. */
    dispatch_async(dispatch_get_main_queue(), ^{
        [self setNeedsDisplay:YES];
    });
}

- (void)setTvAspect:(BOOL)tv { _tv = tv; [self setNeedsDisplay:YES]; }
- (void)setSmooth:(BOOL)smooth { _smooth = smooth; [self setNeedsDisplay:YES]; }

- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }

- (void)drawRect:(NSRect)dirty
{
    (void)dirty;
    CGContextRef dc = [[NSGraphicsContext currentContext] CGContext];
    const NSRect b = [self bounds];

    CGContextSetRGBFillColor(dc, 0, 0, 0, 1);
    CGContextFillRect(dc, b);

    CGImageRef img = CGBitmapContextCreateImage(_ctx);
    if (!img) return;

    /* 256x212 is not 4:3: the 192 active lines are, and the borders are part
     * of the same field on real hardware, so TV mode stretches the whole
     * buffer rather than letterboxing inside one. */
    const double want = _tv ? (4.0 / 3.0)
                            : (double)COLECOSESSION_FB_WIDTH /
                              (double)COLECOSESSION_FB_HEIGHT;
    double w = b.size.width, h = b.size.height, sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }

    CGContextSetInterpolationQuality(dc, _smooth ? kCGInterpolationHigh
                                                 : kCGInterpolationNone);
    CGContextDrawImage(dc, CGRectMake((w - sw) / 2, (h - sh) / 2, sw, sh), img);
    CGImageRelease(img);
}
@end
