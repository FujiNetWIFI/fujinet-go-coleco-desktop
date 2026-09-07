/*
 * host.c -- see host.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "host.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fujinet_cart.h"

/* ---- state ---------------------------------------------------------------- */

static adamcore *s_core;
static coleco_cart *s_cart;
static pthread_t s_thread;
static atomic_bool s_running;
static atomic_bool s_stop_req;
static atomic_int s_reset_req = -1;
static char s_error[256];

static uint8_t s_os7[ADAMCORE_OS7_ROM_SIZE];
static uint8_t *s_cart_image;
static uint32_t s_cart_size;
static char s_hostport[128];
static int s_have_hostport;
static int s_want_cart;

/* ---- frame slot ----------------------------------------------------------- */

static pthread_mutex_t s_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t s_frame[COLECO_FB_WIDTH * COLECO_FB_HEIGHT];
static uint64_t s_frame_serial;

static void frame_publish(const uint16_t *fb)
{
    pthread_mutex_lock(&s_frame_lock);
    memcpy(s_frame, fb, sizeof s_frame);
    s_frame_serial++;
    pthread_mutex_unlock(&s_frame_lock);
}

int coleco_host_frame_copy(uint16_t *dst, uint64_t *serial_inout)
{
    int changed;
    pthread_mutex_lock(&s_frame_lock);
    changed = (*serial_inout != s_frame_serial);
    if (changed) {
        memcpy(dst, s_frame, sizeof s_frame);
        *serial_inout = s_frame_serial;
    }
    pthread_mutex_unlock(&s_frame_lock);
    return changed;
}

/* ---- vsync phase lock ------------------------------------------------------
 * A frontend that presents on the compositor's vsync feeds ticks here. While
 * they keep arriving at something close to the machine's own rate, the
 * emulator runs one frame per tick and inherits the display's cadence exactly
 * -- which is what removes the slow beat between 59.92 Hz and a 60 Hz panel.
 * The moment they stop (an unmapped window, a tiling WM that never presents,
 * a frontend with no frame clock at all) it falls back to the wall clock
 * without stalling. */

static pthread_mutex_t s_vs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_vs_cond = PTHREAD_COND_INITIALIZER;
static uint64_t s_vs_count;
static int64_t s_vs_last_ns;   /* frontend's frame-clock time, informational */
static int64_t s_vs_seen_ns;   /* our own monotonic time of the last tick */

static int64_t mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

void coleco_host_notify_vsync(int64_t frame_time_ns)
{
    pthread_mutex_lock(&s_vs_lock);
    s_vs_last_ns = frame_time_ns;
    s_vs_seen_ns = mono_ns();
    s_vs_count++;
    pthread_cond_signal(&s_vs_cond);
    pthread_mutex_unlock(&s_vs_lock);
}

/* Are ticks actually arriving? Asked BEFORE blocking on one, and the reason
 * matters: waiting for a tick that is never going to come costs the timeout
 * on every frame, which halves the frame rate for any frontend with no frame
 * clock -- headless tests included. Checking first is what makes the
 * fallback a fallback rather than a penalty. */
static int vsync_recent(void)
{
    int recent;
    pthread_mutex_lock(&s_vs_lock);
    recent = s_vs_seen_ns != 0 && (mono_ns() - s_vs_seen_ns) < 250000000LL;
    pthread_mutex_unlock(&s_vs_lock);
    return recent;
}

/* ---- the clock ------------------------------------------------------------ */

static void add_ns(struct timespec *t, long ns)
{
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L) {
        t->tv_nsec -= 1000000000L;
        t->tv_sec += 1;
    }
}

static long ts_diff_ns(const struct timespec *a, const struct timespec *b)
{
    return (long)((a->tv_sec - b->tv_sec) * 1000000000L +
                  (a->tv_nsec - b->tv_nsec));
}

/* clock_nanosleep(TIMER_ABSTIME) is the right tool on POSIX, but it is a
 * no-op under mingw/Wine -- the emulator then free-runs at thousands of fps
 * in a Windows build -- so Windows uses a plain relative nanosleep, which
 * winpthreads honours. Darwin has no clock_nanosleep at all (a glibc/POSIX.1b
 * extension the BSD-derived libc never picked up) and takes the same path. */
static void sleep_until(const struct timespec *next, const struct timespec *now)
{
#if defined(_WIN32) || defined(__APPLE__)
    long remain = ts_diff_ns(next, now);
    struct timespec rel;
    if (remain <= 0) return;
    rel.tv_sec = remain / 1000000000L;
    rel.tv_nsec = remain % 1000000000L;
    nanosleep(&rel, NULL);
#else
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL);
    (void)now;
#endif
}

/* Wait for the next vsync tick, or give up after `timeout_ns`. Returns 1 if a
 * tick arrived. The timeout is what keeps a frontend that stops presenting
 * from freezing the machine. */
static int wait_vsync(uint64_t *seen, long timeout_ns)
{
    struct timespec deadline;
    int got = 0;

    clock_gettime(CLOCK_REALTIME, &deadline);
    add_ns(&deadline, timeout_ns);

    pthread_mutex_lock(&s_vs_lock);
    while (s_vs_count == *seen) {
        if (pthread_cond_timedwait(&s_vs_cond, &s_vs_lock, &deadline) != 0)
            break;
    }
    if (s_vs_count != *seen) {
        *seen = s_vs_count;
        got = 1;
    }
    pthread_mutex_unlock(&s_vs_lock);
    return got;
}

/* ---- the thread ----------------------------------------------------------- */

static void *machine_thread(void *arg)
{
    /* The exact frame period from the exact clock: 59736 cycles at 3.579545
     * MHz is 59.92 Hz, not 60. */
    const long frame_ns =
        (long)(1e9 * (double)COLECO_CYCLES_PER_FRAME / COLECO_CPU_HZ + 0.5);
    /* Give up on a tick after two frames' worth and fall back to the clock. */
    const long vs_timeout_ns = frame_ns * 2;
    struct timespec next, now;
    uint64_t vs_seen = 0;
    int locked = 0;

    (void)arg;

    /* Bring the cartridge up here rather than on the caller's thread: the TCP
     * connect to the BoIP listener is quick but not free, and a failed link
     * must not stall the UI -- the mailbox runs link-down and the CONFIG
     * client says so on screen. */
    if (s_want_cart) {
        s_cart = coleco_cart_create(s_cart_image, s_cart_size,
                                    s_have_hostport ? s_hostport : NULL);
        if (s_cart)
            adamcore_set_cart_ops(s_core, coleco_cart_ops(), s_cart);
    }

    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!atomic_load(&s_stop_req)) {
        int mode = atomic_exchange(&s_reset_req, -1);
        if (mode >= 0)
            adamcore_request_reset(s_core, mode);

        adamcore_run_frame(s_core);
        frame_publish(adamcore_framebuffer(s_core, NULL, NULL));

        if (vsync_recent() && wait_vsync(&vs_seen, vs_timeout_ns)) {
            /* Phase-locked: the display's cadence is the machine's. Keep the
             * wall-clock ladder rebased so dropping back out is seamless. */
            locked = 1;
            clock_gettime(CLOCK_MONOTONIC, &next);
            continue;
        }
        if (locked) {
            /* Ticks stopped -- the window was hidden, or the compositor
             * stopped presenting. Rebase before falling back, or the ladder
             * is however far in the past the lock began. */
            locked = 0;
            clock_gettime(CLOCK_MONOTONIC, &next);
        }

        /* Absolute deadline ladder; resync when badly behind (a laptop
         * resume, a debugger pause) rather than fast-forwarding through
         * however many frames were missed. */
        add_ns(&next, frame_ns);
        clock_gettime(CLOCK_MONOTONIC, &now);
        {
            long behind = ts_diff_ns(&now, &next);
            if (behind > 4 * frame_ns)
                next = now;
            else if (behind < 0)
                sleep_until(&next, &now);
        }
    }
    return NULL;
}

/* ---- lifecycle ------------------------------------------------------------ */

const char *coleco_host_last_error(void) { return s_error; }
int coleco_host_is_running(void) { return atomic_load(&s_running); }
struct coleco_cart *coleco_host_cart(void) { return s_cart; }
adamcore *coleco_host_core(void) { return s_core; }

int coleco_host_start(const coleco_host_opts *opts)
{
    adamcore_config cfg;

    if (atomic_load(&s_running)) return 0;
    s_error[0] = '\0';

    if (!opts || !opts->os7) {
        snprintf(s_error, sizeof s_error,
                 "No ColecoVision BIOS. Import OS7.rom before starting.");
        return -1;
    }
    memcpy(s_os7, opts->os7, sizeof s_os7);

    free(s_cart_image);
    s_cart_image = NULL;
    s_cart_size = 0;
    if (opts->cart && opts->cart_size) {
        s_cart_image = malloc(opts->cart_size);
        if (!s_cart_image) {
            snprintf(s_error, sizeof s_error, "Out of memory");
            return -1;
        }
        memcpy(s_cart_image, opts->cart, opts->cart_size);
        s_cart_size = opts->cart_size;
    }

    /* An empty string disables the cartridge device entirely -- a bare
     * ColecoVision, which is what -DWITH_FUJINET=OFF and the "no FujiNet"
     * setting produce. NULL takes the default endpoint. */
    s_have_hostport = 0;
    s_want_cart = 1;
    if (opts->fujinet_hostport) {
        if (opts->fujinet_hostport[0] == '\0') {
            s_want_cart = 0;
        } else {
            snprintf(s_hostport, sizeof s_hostport, "%s",
                     opts->fujinet_hostport);
            s_have_hostport = 1;
        }
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.os7_rom_data = s_os7;
    cfg.start_machine = ADAMCORE_MACHINE_CV;
    cfg.sgm = opts->sgm;
    cfg.palette = opts->palette;
    cfg.swap_buttons = opts->swap_buttons;
    cfg.audio_rate = opts->audio_rate > 0 ? opts->audio_rate : 44100;
    /* AdamNet BoIP is the ADAM's transport and has no part here: this machine
     * talks to FujiNet through the cartridge mailbox. */
    cfg.boip_listen_port = 0;
    /* The cartridge image is served by the cart device, not by adamcore's
     * flat window, so cart_path stays unset even when an image is loaded. */

    s_core = adamcore_create(&cfg);
    if (!s_core) {
        snprintf(s_error, sizeof s_error,
                 "adamcore refused the configuration (is OS7.rom 8192 bytes?)");
        return -1;
    }

    atomic_store(&s_stop_req, false);
    atomic_store(&s_reset_req, -1);
    if (pthread_create(&s_thread, NULL, machine_thread, NULL) != 0) {
        snprintf(s_error, sizeof s_error, "Could not start the emulator thread");
        adamcore_destroy(s_core);
        s_core = NULL;
        return -1;
    }
    atomic_store(&s_running, true);
    return 0;
}

void coleco_host_stop(void)
{
    if (!atomic_load(&s_running)) return;
    atomic_store(&s_stop_req, true);
    /* Wake the thread if it is parked waiting for a vsync that will never
     * come, so stopping a hidden window does not take a timeout. */
    coleco_host_notify_vsync(0);
    pthread_join(s_thread, NULL);
    atomic_store(&s_running, false);

    if (s_core) adamcore_set_cart_ops(s_core, NULL, NULL);
    coleco_cart_destroy(s_cart);
    s_cart = NULL;
    adamcore_destroy(s_core);
    s_core = NULL;
    free(s_cart_image);
    s_cart_image = NULL;
    s_cart_size = 0;
}

int coleco_host_render_audio(int16_t *out, int nsamples)
{
    /* adamcore synthesizes on the caller's thread from a timestamped queue
     * and is explicitly audio-thread safe, so there is no ring of our own to
     * go through. When the machine is stopped, hand back silence rather than
     * leaving the device's buffer undefined. */
    if (!s_core) {
        memset(out, 0, (size_t)nsamples * sizeof *out);
        return nsamples;
    }
    return adamcore_render_audio(s_core, out, nsamples);
}

void coleco_host_joystick(int port, uint16_t state)
{
    if (s_core) adamcore_set_joystick(s_core, port, state);
}

void coleco_host_reset(void)
{
    /* Mode 1 is the only reset a ColecoVision has. The cartridge does not see
     * it -- adamcore's cart ops are deliberately not reset here. */
    atomic_store(&s_reset_req, 1);
}
