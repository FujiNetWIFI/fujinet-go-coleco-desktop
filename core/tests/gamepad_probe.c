/*
 * gamepad_probe -- an INTERACTIVE check of the SDL gamepad layer.
 *
 * Not a ctest: it needs a human to press things. The automated suite can
 * prove the encoder is right (input_test does) but nothing on a CI runner can
 * tell you whether a real pad's d-pad arrives as a hat, as buttons, or as
 * axes -- and the answer differs per controller, which is exactly where a
 * mapping bug hides.
 *
 * Run it with a pad plugged in:  ./gamepad_probe [seconds]
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "colecosession.h"

static void describe(uint16_t w, char *out, size_t n)
{
    /* Active-low: a CLEAR bit means pressed. */
    out[0] = '\0';
    if (!(w & 0x0100)) strncat(out, "UP ", n - strlen(out) - 1);
    if (!(w & 0x0400)) strncat(out, "DOWN ", n - strlen(out) - 1);
    if (!(w & 0x0800)) strncat(out, "LEFT ", n - strlen(out) - 1);
    if (!(w & 0x0200)) strncat(out, "RIGHT ", n - strlen(out) - 1);
    if (!(w & 0x4000)) strncat(out, "FIRE-L ", n - strlen(out) - 1);
    if (!(w & 0x0040)) strncat(out, "FIRE-R ", n - strlen(out) - 1);
    if (!out[0]) strncat(out, "(idle)", n - strlen(out) - 1);
}

int main(int argc, char **argv)
{
    colecosession_paths paths;
    colecosession_start_opts opts;
    colecosession *s;
    char name[128] = "";
    uint16_t last[2] = { 0x7F7F, 0x7F7F };
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    int i, npads, seen = 0;
    /* What we have proof of, so the summary is about coverage, not vibes. */
    int got_up = 0, got_down = 0, got_left = 0, got_right = 0;
    int got_fl = 0, got_fr = 0;

    /* Line-buffered: this is a live probe and its output is routinely
     * redirected to a file that someone watches with tail. Block buffering
     * would hold everything until exit, which is exactly the wrong shape for
     * a tool you are meant to watch while pressing things. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    memset(&paths, 0, sizeof paths);
    paths.fujinet_lib = "";
    s = colecosession_new(&paths);
    if (!s) { fprintf(stderr, "session failed\n"); return 1; }

    colecosession_default_opts(s, &opts);
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 1;
    opts.cart_path = NULL;
    /* No BIOS needed: the pad layer runs whether or not the machine booted,
     * and this is about the pad, not the emulator. */
    if (colecosession_start(s, &opts) != 0)
        fprintf(stderr, "note: machine did not start (%s) -- "
                        "the pad layer is still under test\n",
                colecosession_last_error(s));

    { struct timespec t = { 0, 300000000 }; nanosleep(&t, NULL); }

    npads = colecosession_gamepad_count(s);
    colecosession_gamepad_name(s, 0, name, sizeof name);
    printf("pads the session opened: %d\n", npads);
    printf("port 1: \"%s\"\n", name[0] ? name : "(none)");
    if (npads < 1) {
        fprintf(stderr, "\nNo pad. Either none is plugged in, or SDL has no "
                        "mapping for it (it would then be a joystick, not a "
                        "gamepad).\n");
        colecosession_stop(s);
        colecosession_free(s);
        return 1;
    }

    printf("\nPress the d-pad in all four directions and both fire buttons.\n"
           "Watching for %d seconds...\n\n", seconds);

    for (i = 0; i < seconds * 20; i++) {
        struct timespec t = { 0, 50000000 };
        int p;
        for (p = 0; p < 2; p++) {
            const uint16_t w = colecosession_gamepad_last_state(s, p);
            if (w != last[p]) {
                char desc[96];
                describe(w, desc, sizeof desc);
                printf("  port %d  %04X  %s\n", p + 1, w, desc);
                last[p] = w;
                seen++;
                if (!(w & 0x0100)) got_up = 1;
                if (!(w & 0x0400)) got_down = 1;
                if (!(w & 0x0800)) got_left = 1;
                if (!(w & 0x0200)) got_right = 1;
                if (!(w & 0x4000)) got_fl = 1;
                if (!(w & 0x0040)) got_fr = 1;
                fflush(stdout);
            }
        }
        nanosleep(&t, NULL);
    }

    printf("\n%d state changes seen.\n", seen);
    printf("  up %s   down %s   left %s   right %s   fire-L %s   fire-R %s\n",
           got_up ? "OK" : "-- ", got_down ? "OK" : "-- ",
           got_left ? "OK" : "-- ", got_right ? "OK" : "-- ",
           got_fl ? "OK" : "-- ", got_fr ? "OK" : "-- ");
    if (got_up && got_down && got_left && got_right && got_fl && got_fr)
        printf("\ngamepad_probe: every direction and both fire buttons "
               "reached the machine.\n");
    else
        printf("\ngamepad_probe: not everything was exercised (see above).\n");

    colecosession_stop(s);
    colecosession_free(s);
    return 0;
}
