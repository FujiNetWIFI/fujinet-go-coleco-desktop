/*
 * DisplayGlue.h -- what DisplayView.m needs to know about THIS machine: the
 * session's functions, the frame size and the shape of the picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "colecosession.h"

#define FN_VIEW      ColecoDisplayView
#define FN_SESSION   colecosession
#define FN_FB_W      COLECOSESSION_FB_WIDTH
#define FN_FB_H      COLECOSESSION_FB_HEIGHT
#define FN_INITIAL_W 848
#define FN_INITIAL_H 636
#define FN_ACCEPTS_FIRST_RESPONDER 1

/* The machine runs 59.92 Hz; the session has no refresh-rate call. */
static inline int fn_machine_hz(FN_SESSION *s) { (void)s; return 60; }

static inline void fn_notify_vsync(FN_SESSION *s, int64_t ns)
{
    colecosession_notify_vsync(s, ns);
}

/* The session's frames are RGB565; the view wants 0x00RRGGBB. Only the
 * display link's thread calls this. */
static inline int fn_pull_frame(FN_SESSION *s, uint32_t *dst, int *lines, uint64_t *serial)
{
    static uint16_t src[COLECOSESSION_FB_WIDTH * COLECOSESSION_FB_HEIGHT];
    if (!colecosession_copy_frame(s, src, serial))
        return 0;
    for (int i = 0; i < COLECOSESSION_FB_WIDTH * COLECOSESSION_FB_HEIGHT; i++) {
        const uint16_t p = src[i];
        unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
        /* Replicate the high bits rather than shifting left: a plain shift
         * maps full-scale 0x1F to 0xF8, so white comes out slightly grey. */
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        dst[i] = (r << 16) | (g << 8) | b;
    }
    *lines = COLECOSESSION_FB_HEIGHT;
    return 1;
}

/* 256x212 is not 4:3: the 192 active lines are, and the borders are part of
 * the same field on real hardware, so TV mode stretches the whole buffer
 * rather than letterboxing inside one. */
static inline double fn_picture_aspect(FN_SESSION *s, int lines, BOOL tv)
{
    (void)s;
    return tv ? (4.0 / 3.0) : (double)COLECOSESSION_FB_WIDTH / (double)lines;
}
