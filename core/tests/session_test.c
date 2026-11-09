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
#ifdef _WIN32
#include <direct.h>
#include <process.h>
/* MSVCRT spells these with a leading underscore. */
#define access _access
#define getpid _getpid
#define R_OK 4
#define W_OK 2
#else
#include <unistd.h>
#endif

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
    colecosession *sess;
    char cfg[600], data[600];
    const char *romsrc = argc > 1 ? argv[1] : NULL;

    snprintf(tmpdir, sizeof tmpdir, "/tmp/coleco-session-test-%d", (int)getpid());
    snprintf(cfg, sizeof cfg, "%s/config", tmpdir);
    snprintf(data, sizeof data, "%s/data", tmpdir);

    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = ""; /* no FujiNet runtime in this test */

    sess = colecosession_new(&paths);
    if (!sess) { fprintf(stderr, "colecosession_new failed\n"); return 1; }

    printf("paths and settings:\n");
    check("the config directory is the one we asked for",
          strcmp(colecosession_config_path(sess), cfg) == 0);
    check("a ROM directory exists", colecosession_roms_path(sess)[0] != '\0');
    check("a cartridge directory exists", colecosession_carts_path(sess)[0] != '\0');

    check("an unset int returns the default",
          colecosession_get_int(sess, "nonexistent", 42) == 42);
    colecosession_set_int(sess, "palette", 2);
    check("an int round trips", colecosession_get_int(sess, "palette", 0) == 2);
    colecosession_set_str(sess, "cart_path", "/some/where.col");
    check("a string round trips",
          strcmp(colecosession_get_str(sess, "cart_path", ""), "/some/where.col") == 0);
    colecosession_settings_flush(sess);

    printf("the input API on a session that never started:\n");
    {
        char text[64];
        int act = -1;
        check("there are 38 binding targets",
              COLECO_TARGET_COUNT == 38 && COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET_CONFIG) == 37);
        check("no controller holds anything",
              colecosession_buttons_held(sess, 0) == 0 && colecosession_buttons_held(sess, 1) == 0);
        check("both controllers read idle",
              colecosession_controller_word(sess, 0) == 0x7F7F && colecosession_controller_word(sess, 1) == 0x7F7F);
        colecosession_press(sess, COLECO_TARGET_PORT(1, COLECO_ACT_FIRE_R), 1);
        check("a press is held without a machine",
              colecosession_buttons_held(sess, 1) == (1u << COLECO_ACT_FIRE_R)
              && colecosession_controller_word(sess, 1) == 0x7F3F);
        colecosession_release_all(sess);
        check("release_all lets go of it", colecosession_buttons_held(sess, 1) == 0);
        check("no gamepads, generation 0, no event",
              colecosession_gamepad_count(sess) == 0 && colecosession_gamepad_generation(sess) == 0
              && colecosession_gamepad_last_event(sess, text, sizeof text) == 0);
        check("assign, assignment and capture are no-ops",
              (colecosession_gamepad_assign(sess, 0, 1), colecosession_gamepad_assignment(sess, 0) == -1)
              && colecosession_gamepad_effective_port(sess, 0) == -1
              && (colecosession_gamepad_capture_begin(sess), colecosession_gamepad_capture_poll(sess, &act) == 0));
        colecosession_gamepad_capture_cancel(sess);
        check("the console action queue is empty", colecosession_sysaction_take(sess, &act) == 0);
    }

    printf("defaults:\n");
    {
        colecosession_start_opts startopts;
        colecosession_default_opts(sess, &startopts);
        /* The SGM defaults ON: it is passive, a cartridge that never touches
         * $50-$53 cannot tell it is fitted, and the titles that need it are
         * otherwise silently wrong. */
        check("the Super Game Module is fitted by default", startopts.sgm == 1);
        check("the persisted palette is picked up", startopts.palette == 2);
        check("the persisted cart path is picked up",
              startopts.cart_path && strcmp(startopts.cart_path, "/some/where.col") == 0);
    }

    /* Settings must survive a new session -- this is what makes one store
     * shared by every frontend of the target mean anything. */
    colecosession_free(sess);
    sess = colecosession_new(&paths);
    if (!sess) { fprintf(stderr, "second colecosession_new failed\n"); return 1; }
    check("settings persist across sessions",
          colecosession_get_int(sess, "palette", 0) == 2);

    printf("the BIOS gate:\n");
    if (!colecosession_bios_available(sess)) {
        colecosession_start_opts startopts;
        colecosession_default_opts(sess, &startopts);
        startopts.enable_fujinet = 0;
        startopts.enable_audio = 0;
        startopts.enable_gamepad = 0;
        startopts.cart_path = NULL;
        check("starting without a BIOS fails rather than showing a black screen",
              colecosession_start(sess, &startopts) != 0);
        check("...and says so", colecosession_last_error(sess)[0] != '\0');
        printf("      (\"%s\")\n", colecosession_last_error(sess));
    } else {
        printf("      a BIOS is already available; gate not exercised\n");
    }

    /* Import a real BIOS if one was supplied, then actually run. */
    if (romsrc) {
        char src[700];
        snprintf(src, sizeof src, "%s/OS7.rom", romsrc);
        if (access(src, R_OK) == 0) {
            colecosession_start_opts startopts;
            int impres = colecosession_import_bios(sess, src);
            check("importing a real BIOS succeeds", impres >= 0);
            check("the BIOS is then available", colecosession_bios_available(sess));

            colecosession_default_opts(sess, &startopts);
            startopts.enable_fujinet = 0;   /* no runtime in this test */
            startopts.enable_audio = 0;     /* no audio device in CI */
            startopts.enable_gamepad = 0;
            startopts.cart_path = NULL;
            check("the session starts", colecosession_start(sess, &startopts) == 0);
            check("...and reports running", colecosession_is_running(sess));
            if (colecosession_is_running(sess)) {
                uint16_t *framebuf = malloc(sizeof(uint16_t) *
                                      COLECOSESSION_FB_WIDTH *
                                      COLECOSESSION_FB_HEIGHT);
                uint64_t serial = 0;
                int tries = 0, got = 0;
                while (tries++ < 200 && !got) {
                    struct timespec ts = { 0, 10000000 };
                    got = colecosession_copy_frame(sess, framebuf, &serial);
                    if (!got) nanosleep(&ts, NULL);
                }
                check("a frame reaches the frontend", got);
                free(framebuf);
            }
            colecosession_stop(sess);
            check("...and stops", !colecosession_is_running(sess));
        } else {
            printf("      no OS7.rom in %s; run path not exercised\n", romsrc);
        }
    }

    colecosession_free(sess);
    rm_rf(tmpdir);

    if (failures) {
        fprintf(stderr, "\nsession_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("\nsession_test: PASS\n");
    return 0;
}
