/*
 * session_test -- the frontend contract, exercised the way a frontend uses it.
 *
 * Runs against a real temporary XDG tree rather than mocks: the path layer,
 * the settings file and the BIOS gate are exactly the parts a unit test with
 * fixtures would skip, and they are where a fresh install actually goes
 * wrong.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "colecosession.h"

static int failures;

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok " : "FAIL", what);
    if (!ok) failures++;
}

static char tmpdir[512];

static void rm_rf(const char *path)
{
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
    if (system(cmd) != 0) { /* best effort */ }
}

int main(int argc, char **argv)
{
    colecosession_paths paths;
    colecosession *s;
    char cfg[600], data[600];
    const char *romsrc = argc > 1 ? argv[1] : NULL;

    snprintf(tmpdir, sizeof tmpdir, "/tmp/coleco-session-test-%d", (int)getpid());
    snprintf(cfg, sizeof cfg, "%s/config", tmpdir);
    snprintf(data, sizeof data, "%s/data", tmpdir);

    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = ""; /* no FujiNet runtime in this test */

    s = colecosession_new(&paths);
    if (!s) { fprintf(stderr, "colecosession_new failed\n"); return 1; }

    printf("paths and settings:\n");
    check("the config directory is the one we asked for",
          strcmp(colecosession_config_path(s), cfg) == 0);
    check("a ROM directory exists", colecosession_roms_path(s)[0] != '\0');
    check("a cartridge directory exists", colecosession_carts_path(s)[0] != '\0');

    check("an unset int returns the default",
          colecosession_get_int(s, "nonexistent", 42) == 42);
    colecosession_set_int(s, "palette", 2);
    check("an int round trips", colecosession_get_int(s, "palette", 0) == 2);
    colecosession_set_str(s, "cart_path", "/some/where.col");
    check("a string round trips",
          strcmp(colecosession_get_str(s, "cart_path", ""), "/some/where.col") == 0);
    colecosession_settings_flush(s);

    printf("defaults:\n");
    {
        colecosession_start_opts o;
        colecosession_default_opts(s, &o);
        /* The SGM defaults ON: it is passive, a cartridge that never touches
         * $50-$53 cannot tell it is fitted, and the titles that need it are
         * otherwise silently wrong. */
        check("the Super Game Module is fitted by default", o.sgm == 1);
        check("the persisted palette is picked up", o.palette == 2);
        check("the persisted cart path is picked up",
              o.cart_path && strcmp(o.cart_path, "/some/where.col") == 0);
    }

    /* Settings must survive a new session -- this is what makes one store
     * shared by every frontend of the target mean anything. */
    colecosession_free(s);
    s = colecosession_new(&paths);
    if (!s) { fprintf(stderr, "second colecosession_new failed\n"); return 1; }
    check("settings persist across sessions",
          colecosession_get_int(s, "palette", 0) == 2);

    printf("the BIOS gate:\n");
    if (!colecosession_bios_available(s)) {
        colecosession_start_opts o;
        colecosession_default_opts(s, &o);
        o.enable_fujinet = 0;
        o.enable_audio = 0;
        o.enable_gamepad = 0;
        o.cart_path = NULL;
        check("starting without a BIOS fails rather than showing a black screen",
              colecosession_start(s, &o) != 0);
        check("...and says so", colecosession_last_error(s)[0] != '\0');
        printf("      (\"%s\")\n", colecosession_last_error(s));
    } else {
        printf("      a BIOS is already available; gate not exercised\n");
    }

    /* Import a real BIOS if one was supplied, then actually run. */
    if (romsrc) {
        char src[700];
        snprintf(src, sizeof src, "%s/OS7.rom", romsrc);
        if (access(src, R_OK) == 0) {
            colecosession_start_opts o;
            int rc = colecosession_import_bios(s, src);
            check("importing a real BIOS succeeds", rc >= 0);
            check("the BIOS is then available", colecosession_bios_available(s));

            colecosession_default_opts(s, &o);
            o.enable_fujinet = 0;   /* no runtime in this test */
            o.enable_audio = 0;     /* no audio device in CI */
            o.enable_gamepad = 0;
            o.cart_path = NULL;
            check("the session starts", colecosession_start(s, &o) == 0);
            check("...and reports running", colecosession_is_running(s));
            if (colecosession_is_running(s)) {
                uint16_t *fb = malloc(sizeof(uint16_t) *
                                      COLECOSESSION_FB_WIDTH *
                                      COLECOSESSION_FB_HEIGHT);
                uint64_t serial = 0;
                int tries = 0, got = 0;
                while (tries++ < 200 && !got) {
                    struct timespec ts = { 0, 10000000 };
                    got = colecosession_copy_frame(s, fb, &serial);
                    if (!got) nanosleep(&ts, NULL);
                }
                check("a frame reaches the frontend", got);
                free(fb);
            }
            colecosession_stop(s);
            check("...and stops", !colecosession_is_running(s));
        } else {
            printf("      no OS7.rom in %s; run path not exercised\n", romsrc);
        }
    }

    colecosession_free(s);
    rm_rf(tmpdir);

    if (failures) {
        fprintf(stderr, "\nsession_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("\nsession_test: PASS\n");
    return 0;
}
