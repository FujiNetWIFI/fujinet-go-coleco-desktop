/*
 * gamepad_test -- the gamepad layer's pure helpers (no hardware): the stick's
 * hysteresis (0.5 in, 0.35 out), the left stick and D-pad as the controller's
 * directions, port assignment and the SDL button tables; and that the SDL
 * gamepad subsystem starts and stops cleanly with nothing attached, twice.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "gamepad_sdl.h"
#include "session_internal.h"
#include "test_tmpdir.h"

static int failures;
static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

#define BAND  COLECO_STICK_ENTER, COLECO_STICK_EXIT
#define ACT_BIT(act) (1u << (act))

int main(void)
{
    int assign[4];

    /* stick hysteresis: in at half deflection, out below 0.35 */
    check(COLECO_STICK_ENTER == 0.5f && COLECO_STICK_EXIT == 0.35f, "the thresholds are 0.5 and 0.35");
    check(coleco_dir_from_stick(0.f, 0.f, 0, BAND) == 0, "centered stick is no direction");
    check(coleco_dir_from_stick(0.6f, 0.f, 0, BAND) == COLECO_DIR_RIGHT, "past half right engages right");
    check(coleco_dir_from_stick(0.4f, 0.f, 0, BAND) == 0, "two fifths right does not engage");
    check(coleco_dir_from_stick(0.4f, 0.f, COLECO_DIR_RIGHT, BAND) == COLECO_DIR_RIGHT, "but holds once engaged");
    check(coleco_dir_from_stick(0.3f, 0.f, COLECO_DIR_RIGHT, BAND) == 0, "and releases below exit");
    check(coleco_dir_from_stick(-0.6f, -0.6f, 0, BAND) == (COLECO_DIR_LEFT | COLECO_DIR_UP), "diagonals are two bits");
    check(coleco_dir_from_stick(0.6f, 0.05f, 0, BAND) == COLECO_DIR_RIGHT,
          "a pure horizontal picks up no vertical noise");
    check(coleco_dir_from_stick(0.f, 0.9f, COLECO_DIR_RIGHT, BAND) == COLECO_DIR_DOWN,
          "an axis is judged on its own: a stick that left right is down");
    check(coleco_dir_from_stick(0.f, -0.5f, 0, BAND) == COLECO_DIR_UP
          && coleco_dir_from_stick(0.f, -0.49f, 0, BAND) == 0, "exactly half deflection engages");

    /* directions as the controller's actions */
    check(coleco_acts_from_dir(COLECO_DIR_UP) == ACT_BIT(COLECO_ACT_UP) &&
          coleco_acts_from_dir(COLECO_DIR_DOWN) == ACT_BIT(COLECO_ACT_DOWN) &&
          coleco_acts_from_dir(COLECO_DIR_LEFT) == ACT_BIT(COLECO_ACT_LEFT) &&
          coleco_acts_from_dir(COLECO_DIR_RIGHT) == ACT_BIT(COLECO_ACT_RIGHT),
          "the four directions are the stick's four switches");
    check(coleco_acts_from_dir(COLECO_DIR_UP | COLECO_DIR_LEFT) == (ACT_BIT(COLECO_ACT_UP) | ACT_BIT(COLECO_ACT_LEFT)) &&
          coleco_acts_from_dir(COLECO_DIR_DOWN | COLECO_DIR_RIGHT) == (ACT_BIT(COLECO_ACT_DOWN) | ACT_BIT(COLECO_ACT_RIGHT)),
          "a diagonal is two switches");
    check(coleco_acts_from_dir(COLECO_DIR_UP | COLECO_DIR_DOWN) == 0 && coleco_acts_from_dir(0) == 0 &&
          coleco_acts_from_dir(COLECO_DIR_LEFT | COLECO_DIR_RIGHT) == 0,
          "opposites cancel");
    check(coleco_acts_from_dir(COLECO_DIR_UP | COLECO_DIR_DOWN | COLECO_DIR_LEFT) == ACT_BIT(COLECO_ACT_LEFT),
          "a cancelled axis leaves the other one alone");

    /* port assignment */
    assign[0] = -1; assign[1] = -1; assign[2] = -1;
    check(coleco_pad_for_port(assign, 2, 0) == 0 && coleco_pad_for_port(assign, 2, 1) == 1,
          "connection order: first pad controller 1, second pad controller 2");
    check(coleco_pad_for_port(assign, 1, 1) == -1, "one pad leaves controller 2 to the keyboard");
    check(coleco_pad_for_port(assign, 0, 0) == -1, "no pads, no port");
    assign[0] = 1; assign[1] = -1;
    check(coleco_pad_for_port(assign, 2, 1) == 0 && coleco_pad_for_port(assign, 2, 0) == 1,
          "an explicit assignment wins and the other pad fills the gap");
    assign[0] = 1; assign[1] = 1;
    check(coleco_pad_for_port(assign, 2, 1) == 0 && coleco_pad_for_port(assign, 2, 0) == -1,
          "two pads on one port: the first wins, the other port stays empty");
    assign[0] = -1; assign[1] = 0; assign[2] = -1;
    check(coleco_pad_for_port(assign, 3, 0) == 1 && coleco_pad_for_port(assign, 3, 1) == 2,
          "with port 0 taken by pad 1, the unassigned pads fill in order: the second of them gets port 1");

    /* SDL button tables round-trip */
    {
        int btn, pass = 1;
        for (btn = 0; btn < COLECO_PAD_BTN_LEFT_TRIGGER; btn++) {
            int sdl = coleco_sdl_button_from_pad(btn);
            if (sdl < 0 || coleco_pad_button_from_sdl((uint8_t)sdl) != btn) pass = 0;
        }
        check(pass, "every named button round-trips through the SDL tables");
        check(coleco_pad_button_from_sdl(200) == COLECO_PAD_BTN_NONE, "an unknown SDL button is NONE");
        check(coleco_sdl_button_from_pad(COLECO_PAD_BTN_LEFT_TRIGGER) == -1
              && coleco_sdl_button_from_pad(COLECO_PAD_BTN_RAW_BASE) == -1,
              "triggers and raw buttons are not SDL gamepad buttons");
    }

    /* the subsystem with no hardware, no machine: the layer starts, reports
     * its API sanely, and stops -- twice over, and on a session free */
    {
        char cfg[512], data[512], text[64];
        colecosession_paths paths;
        colecosession *sess;
        int round;

        test_tmpdir(cfg, sizeof cfg, "gcfg");
        test_tmpdir(data, sizeof data, "gdat");
        memset(&paths, 0, sizeof paths);
        paths.config_dir = cfg;
        paths.data_dir = data;
        paths.fujinet_lib = "";
        sess = colecosession_new(&paths);
        if (!sess) return 1;

        check(colecosession_gamepad_count(sess) == 0 && colecosession_gamepad_generation(sess) == 0
              && colecosession_gamepad_last_event(sess, text, sizeof text) == 0 && !text[0],
              "before it starts: no pads, generation 0, no event");
        colecosession_gamepad_assign(sess, 0, 1);
        check(colecosession_gamepad_assignment(sess, 0) == -1 && colecosession_gamepad_effective_port(sess, 0) == -1,
              "assigning a pad that is not there does nothing");

        for (round = 0; round < 2; round++) {
            if (gamepad_start(sess) != 0) {
                printf("SKIP: SDL's gamepad subsystem would not start here\n");
                colecosession_free(sess);
                return 77;
            }
            check(gamepad_start(sess) == 0, "starting twice is harmless");
            check(colecosession_gamepad_count(sess) >= 0, "gamepad count answers");
            check(colecosession_gamepad_effective_port(sess, 99) == -1, "an absent pad has no port");
            check(colecosession_gamepad_assignment(sess, -1) == -1, "nor an explicit one");
            colecosession_gamepad_capture_begin(sess);
            check(colecosession_gamepad_capture_poll(sess, NULL) == 0, "nothing captured with no pad");
            colecosession_gamepad_capture_cancel(sess);
            gamepad_stop(sess);
            gamepad_stop(sess);
            check(colecosession_gamepad_count(sess) == 0, "stopped: no pads");
        }
        gamepad_start(sess);
        colecosession_free(sess);      /* frees the layer even though no machine ever ran */
        check(1, "a session freed with its gamepad layer up stops cleanly");
    }

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
