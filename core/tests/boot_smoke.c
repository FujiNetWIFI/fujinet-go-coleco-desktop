/*
 * boot_smoke -- the machine boots, paints, and paces.
 *
 * Runs the real host thread with a real BIOS and asserts three things the
 * sibling ports each got wrong at least once: that a frame is actually
 * published (not just that start() returned 0), that the picture is a
 * picture rather than one flat colour, and that the emulator is THROTTLED --
 * an unthrottled core is the trap every port in this family has fallen into,
 * and it looks like success until the fan comes on.
 *
 * SKIPs (ctest 77) with no BIOS, which is every CI and release checkout:
 * this repository does not redistribute one. Point it at a directory holding
 * OS7.rom with argv[1] or $COLECO_TEST_ROMS.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"

#define SKIP 77

static uint8_t os7[ADAMCORE_OS7_ROM_SIZE];

static int load_os7(const char *dir)
{
    char path[512];
    FILE *f;
    size_t n;
    if (!dir || !*dir) return 0;
    snprintf(path, sizeof path, "%s/OS7.rom", dir);
    f = fopen(path, "rb");
    if (!f) return 0;
    n = fread(os7, 1, sizeof os7, f);
    fclose(f);
    return n == sizeof os7;
}

static int64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

int main(int argc, char **argv)
{
    coleco_host_opts opts;
    uint16_t *fb;
    uint64_t serial = 0;
    int64_t t0, elapsed_ms;
    int frames = 0, i, distinct = 0;
    uint16_t seen[16];
    int nseen = 0;

    if (!load_os7(argc > 1 ? argv[1] : NULL) &&
        !load_os7(getenv("COLECO_TEST_ROMS")) &&
        !load_os7("tools/roms") &&
        !load_os7("../tools/roms")) {
        fprintf(stderr, "boot_smoke: no OS7.rom -- skipping "
                        "(this repository does not redistribute one)\n");
        return SKIP;
    }

    memset(&opts, 0, sizeof opts);
    opts.os7 = os7;
    opts.audio_rate = 44100;
    /* No cartridge device: this is the bare machine, so the test needs no
     * network and cannot be perturbed by whatever is on the loopback port. */
    opts.fujinet_hostport = "";

    if (coleco_host_start(&opts) != 0) {
        fprintf(stderr, "boot_smoke: start failed: %s\n",
                coleco_host_last_error());
        return 1;
    }

    fb = malloc(sizeof(uint16_t) * COLECO_FB_WIDTH * COLECO_FB_HEIGHT);
    if (!fb) return 1;

    /* Count published frames over a second of wall clock, with no vsync
     * ticks fed in, so this measures the wall-clock fallback path. */
    t0 = now_ns();
    while ((now_ns() - t0) < 1000000000LL) {
        if (coleco_host_frame_copy(fb, &serial)) frames++;
        struct timespec s = { 0, 1000000 }; /* 1 ms */
        nanosleep(&s, NULL);
    }
    elapsed_ms = (now_ns() - t0) / 1000000;

    printf("published %d frames in %lld ms\n", frames, (long long)elapsed_ms);

    if (frames == 0) {
        fprintf(stderr, "FAIL: the machine never published a frame\n");
        return 1;
    }
    /* The real trap: an unthrottled core free-runs at hundreds or thousands
     * of frames a second and every other check still passes. 59.92 Hz with
     * generous slack for a loaded machine. */
    if (frames > 75) {
        fprintf(stderr, "FAIL: %d fps -- the emulator is not throttled\n",
                frames);
        return 1;
    }
    /* Deliberately close to 59.92 rather than merely "not zero". Waiting on a
     * vsync tick that never arrives used to cost the timeout on every frame
     * and produced a steady 30 fps here -- a bound of 30 would have called
     * that a pass. */
    if (frames < 50) {
        fprintf(stderr, "FAIL: %d fps -- the emulator is not keeping up "
                        "(a wall-clock fallback should run at ~59.92)\n",
                frames);
        return 1;
    }

    /* A picture, not a flat field. The BIOS with no cartridge paints its
     * "TURN GAME OFF BEFORE INSERTING CARTRIDGE" screen. */
    coleco_host_frame_copy(fb, &serial);
    for (i = 0; i < COLECO_FB_WIDTH * COLECO_FB_HEIGHT && nseen < 16; i++) {
        int j, found = 0;
        for (j = 0; j < nseen; j++)
            if (seen[j] == fb[i]) { found = 1; break; }
        if (!found) seen[nseen++] = fb[i];
    }
    distinct = nseen;
    printf("frame uses %d distinct colours\n", distinct);
    if (distinct < 2) {
        fprintf(stderr, "FAIL: the framebuffer is one flat colour\n");
        return 1;
    }

    /* The phase-lock path: feed ticks at ~60 Hz and the machine should follow
     * them rather than its own clock. */
    {
        int locked_frames = 0;
        int64_t t1 = now_ns();
        while ((now_ns() - t1) < 1000000000LL) {
            struct timespec s2 = { 0, 16000000 }; /* ~60 Hz */
            coleco_host_notify_vsync(now_ns());
            if (coleco_host_frame_copy(fb, &serial)) locked_frames++;
            nanosleep(&s2, NULL);
        }
        printf("phase-locked to ~60 Hz ticks: %d frames\n", locked_frames);
        if (locked_frames < 40 || locked_frames > 75) {
            fprintf(stderr, "FAIL: %d frames while phase-locked\n",
                    locked_frames);
            return 1;
        }
    }

    /* Audio pulls without the machine being asked to stop first. */
    {
        int16_t buf[512];
        int n = coleco_host_render_audio(buf, 512);
        if (n != 512) {
            fprintf(stderr, "FAIL: render_audio returned %d, wanted 512\n", n);
            return 1;
        }
    }

    coleco_host_stop();
    if (coleco_host_is_running()) {
        fprintf(stderr, "FAIL: still running after stop\n");
        return 1;
    }
    free(fb);
    printf("\nboot_smoke: PASS\n");
    return 0;
}
