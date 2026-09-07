/*
 * host -- the emulator thread, the frame slot, and the cartridge's lifetime.
 *
 * adamcore is a library with no thread, no clock and no I/O of its own: the
 * host drives adamcore_run_frame(), publishes the framebuffer, and lets the
 * audio device pull samples. This file is that host, minus anything
 * toolkit-specific -- no SDL, no windowing. colecosession wraps it.
 *
 * A singleton, like the cartridge it owns. fujimail's port interface takes no
 * context argument, so there can only be one FujiNet cartridge per process
 * anyway; pretending the machine were instantiable would be a lie the cart
 * device could not keep.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COLECO_HOST_H
#define COLECO_HOST_H

#include <stdint.h>

#include "adamcore.h"

#define COLECO_FB_WIDTH  ADAMCORE_FB_WIDTH
#define COLECO_FB_HEIGHT ADAMCORE_FB_HEIGHT

/* NTSC ColecoVision: 262 lines of 228 T-states at 3.579545 MHz, which is
 * 59736 cycles a frame and 59.92 Hz -- not 60, and the difference is the
 * ~0.1% the audio timeline has to absorb over a long session. */
#define COLECO_CPU_HZ         3579545
#define COLECO_CYCLES_PER_FRAME (262 * 228)

typedef struct {
    const uint8_t *os7;      /* 8K BIOS; NULL means "no BIOS" and start fails */
    const uint8_t *cart;     /* cartridge image, or NULL for the CONFIG client */
    uint32_t cart_size;
    const char *fujinet_hostport; /* "host:port"; NULL = default, "" = no cart */
    int sgm;                 /* fit an Opcode Super Game Module */
    int palette;             /* 0..3 */
    int swap_buttons;
    int audio_rate;
} coleco_host_opts;

/* Starts the machine on its own paced thread. Returns 0, or -1 with
 * coleco_host_last_error() set. */
int coleco_host_start(const coleco_host_opts *opts);
void coleco_host_stop(void);
int coleco_host_is_running(void);
const char *coleco_host_last_error(void);

/* Copies the latest frame into dst (COLECO_FB_WIDTH*HEIGHT uint16 RGB565)
 * iff its serial differs from *serial_inout, updates *serial_inout, and
 * returns 1; returns 0 when unchanged and leaves dst alone. Pass 0 to force
 * a copy after a window map. */
int coleco_host_frame_copy(uint16_t *dst, uint64_t *serial_inout);

/* Feed the UI's frame-clock ticks (CLOCK_MONOTONIC ns). While a steady ~60 Hz
 * stream arrives the emulator phase-locks one frame per tick; otherwise it
 * paces on the wall clock. */
void coleco_host_notify_vsync(int64_t frame_time_ns);

/* Audio-thread safe: adamcore synthesizes from a timestamped write queue, so
 * the device pulls straight through rather than through a ring of our own. */
int coleco_host_render_audio(int16_t *out, int nsamples);

/* Any thread. state is the ColecoVision controller word (active-low, idle
 * 0x7F7F); see coleco_controller_encode(). */
void coleco_host_joystick(int port, uint16_t state);
void coleco_host_reset(void);

/* The cartridge device, for status displays and the debugger; NULL when the
 * machine is stopped or was started with no cartridge. */
struct coleco_cart *coleco_host_cart(void);

/* The core itself, for the debugger. NULL when stopped. */
adamcore *coleco_host_core(void);

#endif
