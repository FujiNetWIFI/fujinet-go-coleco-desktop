/*
 * audioring.h -- the rate-controlled sample ring between the machine thread
 * (producer) and the audio device callback (consumer).
 *
 * The emulator makes one frame of samples per frame of emulated time; the
 * device drains on its own clock. The two never agree exactly (a vsync-locked
 * 60.00 Hz frame clock against the machine's 59.92 Hz, a 44.1 kHz stream
 * resampled to a 48 kHz device), so the producer runs a linear resampler that
 * steps up to 0.5% faster or slower to hold the ring near its target fill.
 * Same design as the atari800 / atari7800 / MS-DOS desktops.
 *
 * Single producer, single consumer, acquire/release indices. Header-only so
 * the test links nothing.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COLECO_AUDIORING_H
#define COLECO_AUDIORING_H

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define AR_FRAMES 16384u            /* power of two */
#define AR_TARGET 2205u             /* 50 ms at 44.1 kHz */
#define AR_MAX_STEP 0.005

typedef struct {
    int16_t buf[AR_FRAMES];
    _Atomic uint32_t rd;
    _Atomic uint32_t wr;
    _Atomic int flush;
    /* producer-only */
    double pos;
    int16_t last;
} audioring;

static inline void audioring_reset(audioring *r)
{
    memset(r->buf, 0, sizeof r->buf);
    atomic_store(&r->rd, 0);
    atomic_store(&r->wr, 0);
    atomic_store(&r->flush, 0);
    r->pos = 0.0;
    r->last = 0;
}

/* Consumer asks the producer's view to be emptied (reset, unpause). */
static inline void audioring_flush(audioring *r) { atomic_store(&r->flush, 1); }

static inline uint32_t audioring_fill(audioring *r)
{
    uint32_t w = atomic_load_explicit(&r->wr, memory_order_acquire);
    uint32_t rd = atomic_load_explicit(&r->rd, memory_order_acquire);
    return (w - rd) & (AR_FRAMES - 1);
}

/* Producer: push `frames` input samples, resampled toward the target fill. */
static inline void audioring_push(audioring *r, const int16_t *in, int frames)
{
    uint32_t w = atomic_load_explicit(&r->wr, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&r->rd, memory_order_acquire);
    double fill = (double)((w - rd) & (AR_FRAMES - 1));
    double err = (fill - (double)AR_TARGET) / (double)AR_TARGET;
    double step = err * AR_MAX_STEP;
    double pos = r->pos;

    if (frames <= 0) return;
    if (step > AR_MAX_STEP) step = AR_MAX_STEP;
    if (step < -AR_MAX_STEP) step = -AR_MAX_STEP;
    step += 1.0;

    while (pos < (double)frames) {
        int i = (int)pos;
        double frac = pos - i;
        int a = i == 0 ? r->last : in[i - 1];
        int b = in[i];
        uint32_t next = (w + 1) & (AR_FRAMES - 1);
        if (next != rd) {
            {
            double v = a + (b - a) * frac;   /* no libm: round half away */
            r->buf[w] = (int16_t)(v >= 0 ? v + 0.5 : v - 0.5);
            }
            w = next;
        }
        pos += step;
    }
    r->pos = pos - (double)frames;
    r->last = in[frames - 1];
    atomic_store_explicit(&r->wr, w, memory_order_release);
}

/* Consumer: copy up to n samples, pad the rest with silence. Returns the
 * number of real samples. */
static inline int audioring_pull(audioring *r, int16_t *out, int n)
{
    uint32_t rd = atomic_load_explicit(&r->rd, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&r->wr, memory_order_acquire);
    int i = 0;

    if (atomic_exchange(&r->flush, 0)) rd = w;
    for (; i < n && rd != w; i++) {
        out[i] = r->buf[rd];
        rd = (rd + 1) & (AR_FRAMES - 1);
    }
    if (i < n) memset(out + i, 0, (size_t)(n - i) * sizeof *out);
    atomic_store_explicit(&r->rd, rd, memory_order_release);
    return i;
}

#endif
