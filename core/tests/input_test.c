/*
 * input_test -- the key-to-controller path, pinned to exact words.
 *
 * Exact values rather than "something changed", because the controller word
 * is active-low with the directions and the two fire buttons split across
 * both bytes, and a sign error there produces a machine that reads as though
 * every direction is held at once -- which looks like a broken emulator, not
 * a broken table. Session-level: the keyboard, the window's mouse and the
 * merge between them, all without a machine (the push to it is then a no-op
 * and the held state still reads back), so it runs on a box with no BIOS.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "colecosession.h"
#include "test_tmpdir.h"

#define KEY_UP     COLECOSESSION_KEYSYM_UP
#define KEY_DOWN   COLECOSESSION_KEYSYM_DOWN
#define KEY_LEFT   COLECOSESSION_KEYSYM_LEFT
#define KEY_RIGHT  COLECOSESSION_KEYSYM_RIGHT
#define KEY_CTRL   COLECOSESSION_KEYSYM_LCTRL
#define KEY_ALT    COLECOSESSION_KEYSYM_LALT
#define KEY_SHIFT  COLECOSESSION_KEYSYM_LSHIFT
#define KEY_TAB    COLECOSESSION_KEYSYM_TAB

#define BIT(act) (1u << (act))

static int failures;

static void same(const char *what, unsigned got, unsigned want)
{
    if (got == want) {
        printf("  ok  %-52s 0x%04X\n", what, got);
    } else {
        printf("  FAIL %-52s got 0x%04X want 0x%04X\n", what, got, want);
        failures++;
    }
}

static void check(int pass, const char *what)
{
    printf("  %s %s\n", pass ? "ok " : "FAIL", what);
    if (!pass) failures++;
}

static uint16_t word(colecosession *sess, int port)
{
    return colecosession_controller_word(sess, port);
}

static void encoder(void)
{
    printf("the encoder:\n");
    same("idle", coleco_controller_encode(0,0,0,0,0,0,-1), 0x7F7F);
    same("up", coleco_controller_encode(1,0,0,0,0,0,-1), 0x7E7F);
    same("right", coleco_controller_encode(0,0,0,1,0,0,-1), 0x7D7F);
    same("down", coleco_controller_encode(0,1,0,0,0,0,-1), 0x7B7F);
    same("left", coleco_controller_encode(0,0,1,0,0,0,-1), 0x777F);
    same("left fire", coleco_controller_encode(0,0,0,0,1,0,-1), 0x3F7F);
    same("right fire", coleco_controller_encode(0,0,0,0,0,1,-1), 0x7F3F);
    same("keypad 0", coleco_controller_encode(0,0,0,0,0,0,0), 0x7F70);
    same("keypad 5", coleco_controller_encode(0,0,0,0,0,0,5), 0x7F75);
    same("keypad * (10)", coleco_controller_encode(0,0,0,0,0,0,10), 0x7F7A);
    same("keypad # (11)", coleco_controller_encode(0,0,0,0,0,0,11), 0x7F7B);
    /* Out of range must read as no key, not wrap into a real one. */
    same("keypad 12 is no key", coleco_controller_encode(0,0,0,0,0,0,12), 0x7F7F);
    same("keypad -1 is no key", coleco_controller_encode(0,0,0,0,0,0,-1), 0x7F7F);
    same("up + right fire together", coleco_controller_encode(1,0,0,0,0,1,-1), 0x7E3F);
    same("right + down + left fire", coleco_controller_encode(0,1,0,1,1,0,-1), 0x397F);
}

static void keyboard(colecosession *sess)
{
    printf("the keyboard:\n");
    same("idle on controller 1", word(sess, 0), 0x7F7F);
    same("idle on controller 2", word(sess, 1), 0x7F7F);

    check(colecosession_key(sess, KEY_UP, 1) == 1, "Up is bound");
    same("Up held on controller 1", word(sess, 0), 0x7E7F);
    same("...leaves controller 2 idle", word(sess, 1), 0x7F7F);
    same("buttons_held agrees", colecosession_buttons_held(sess, 0), BIT(COLECO_ACT_UP));
    colecosession_key(sess, KEY_UP, 0);
    same("Up released", word(sess, 0), 0x7F7F);

    colecosession_key(sess, KEY_LEFT, 1);
    colecosession_key(sess, KEY_CTRL, 1);
    same("Left + Ctrl", word(sess, 0), 0x377F);
    colecosession_key(sess, KEY_ALT, 1);
    same("Left + both fires", word(sess, 0), 0x373F);
    colecosession_key(sess, KEY_LEFT, 0);
    colecosession_key(sess, KEY_CTRL, 0);
    colecosession_key(sess, KEY_ALT, 0);
    same("all released", word(sess, 0), 0x7F7F);

    colecosession_key(sess, 'w', 1);
    colecosession_key(sess, KEY_SHIFT, 1);
    same("W + Shift on controller 2", word(sess, 1), 0x3E7F);
    same("...leaves controller 1 idle", word(sess, 0), 0x7F7F);
    colecosession_key(sess, 'w', 0);
    colecosession_key(sess, KEY_SHIFT, 0);
    colecosession_key(sess, KEY_TAB, 1);
    same("Tab is controller 2's right fire", word(sess, 1), 0x7F3F);
    colecosession_key(sess, KEY_TAB, 0);

    colecosession_key(sess, KEY_RIGHT, 1);
    colecosession_key(sess, KEY_DOWN, 1);
    colecosession_key(sess, KEY_CTRL, 1);
    same("right + down + left fire", word(sess, 0), 0x397F);
    colecosession_release_all(sess);
    same("release_all lets go of the keyboard's", word(sess, 0), 0x7F7F);
    colecosession_key(sess, KEY_RIGHT, 0);
    same("a late key-up of a released key is harmless", word(sess, 0), 0x7F7F);

    check(colecosession_key(sess, 0xFF1B /* Escape */, 1) == 0, "Escape is bound to nothing");
    colecosession_key(sess, KEY_UP, 1);
    check(colecosession_key(sess, 0, 0) == 0 && colecosession_key(sess, 0, 1) == 0
          && colecosession_buttons_held(sess, 0) == BIT(COLECO_ACT_UP),
          "keysym 0 is not a key: its release does not let go of what is held");
    colecosession_key(sess, KEY_UP, 0);
    check(colecosession_key(sess, 0xFFC2 /* F5 */, 1) == 0 && word(sess, 0) == 0x7F7F,
          "a console action key is not consumed as a controller input");
}

static void keypad(colecosession *sess)
{
    printf("the keypad is held, one key at a time:\n");
    colecosession_key(sess, '5', 1);
    same("5 held", word(sess, 0), 0x7F75);
    /* Rolling from one digit onto another without releasing the first
     * leaves a key held, not none: a game polling once a frame would
     * otherwise see the gap and drop the press. And the console reads one
     * key, so the lowest wins. */
    colecosession_key(sess, '7', 1);
    same("5 and 7 held: the lowest, 5, is what the console sees", word(sess, 0), 0x7F75);
    same("buttons_held says the same", colecosession_buttons_held(sess, 0), BIT(COLECO_ACT_KEYPAD + 5));
    colecosession_key(sess, '5', 0);
    same("releasing 5 leaves 7", word(sess, 0), 0x7F77);
    colecosession_key(sess, '3', 1);
    same("hold 7 then 3: 3", word(sess, 0), 0x7F73);
    colecosession_key(sess, '3', 0);
    same("release 3: 7 is back", word(sess, 0), 0x7F77);
    colecosession_key(sess, '7', 0);
    same("releasing 7 clears it", word(sess, 0), 0x7F7F);

    colecosession_key(sess, COLECOSESSION_KEYSYM_KP_MULTIPLY, 1);
    same("Keypad * is *", word(sess, 0), 0x7F7A);
    colecosession_key(sess, COLECOSESSION_KEYSYM_KP_MULTIPLY, 0);
    colecosession_key(sess, COLECOSESSION_KEYSYM_KP_DIVIDE, 1);
    same("Keypad / is #", word(sess, 0), 0x7F7B);
    colecosession_key(sess, COLECOSESSION_KEYSYM_KP_DIVIDE, 0);

    colecosession_key(sess, COLECOSESSION_KEYSYM_KP_5, 1);
    same("the numeric keypad dials too", word(sess, 0), 0x7F75);
    colecosession_key(sess, COLECOSESSION_KEYSYM_KP_5, 0);
    same("...and releases", word(sess, 0), 0x7F7F);

    colecosession_key(sess, '5', 1);
    colecosession_key(sess, COLECOSESSION_KEYSYM_KP_5, 1);
    colecosession_key(sess, '5', 0);
    same("the digit's release does not let go of the numpad key's hold",
         word(sess, 0), 0x7F75);
    colecosession_key(sess, COLECOSESSION_KEYSYM_KP_5, 0);
    same("and the last release clears it", word(sess, 0), 0x7F7F);
}

static void pairs(colecosession *sess)
{
    printf("opposed directions cancel:\n");
    colecosession_key(sess, KEY_UP, 1);
    colecosession_key(sess, KEY_DOWN, 1);
    same("Up + Down hold neither", word(sess, 0), 0x7F7F);
    colecosession_key(sess, KEY_LEFT, 1);
    same("...and the other axis is untouched", word(sess, 0), 0x777F);
    colecosession_key(sess, KEY_UP, 0);
    same("Down is back once Up lets go", word(sess, 0), 0x737F);
    colecosession_key(sess, KEY_RIGHT, 1);
    same("Left + Right hold neither (Down is still there)", word(sess, 0), 0x7B7F);
    colecosession_release_all(sess);
    same("released", word(sess, 0), 0x7F7F);
}

static void sources(colecosession *sess)
{
    printf("the sources do not step on each other:\n");
    colecosession_key(sess, KEY_UP, 1);
    colecosession_pointer_set(sess, 0, BIT(COLECO_ACT_UP) | BIT(COLECO_ACT_FIRE_L));
    same("a key and the mouse both on Up", word(sess, 0), 0x3E7F);
    colecosession_key(sess, KEY_UP, 0);
    same("releasing the key leaves the mouse's Up and fire", word(sess, 0), 0x3E7F);
    colecosession_pointer_set(sess, 0, 0);
    same("letting go of the mouse clears it", word(sess, 0), 0x7F7F);

    colecosession_pointer_set(sess, 1, BIT(COLECO_ACT_KEYPAD + 9));
    same("the mouse on controller 2's 9", word(sess, 1), 0x7F79);
    colecosession_key(sess, 'a', 1);
    colecosession_release_all(sess);
    same("release_all leaves the mouse's hold", word(sess, 1), 0x7F79);
    colecosession_pointer_set(sess, 1, 0);
    same("and the pointer lets go of it", word(sess, 1), 0x7F7F);

    colecosession_press(sess, COLECO_TARGET_PORT(1, COLECO_ACT_FIRE_L), 1);
    same("press() holds a target", word(sess, 1), 0x3F7F);
    colecosession_press(sess, COLECO_TARGET_PORT(1, COLECO_ACT_FIRE_L), 0);
    same("...and releases it", word(sess, 1), 0x7F7F);

    /* the same key bound to two targets presses both */
    colecosession_binding_set_key(sess, COLECO_TARGET_PORT(0, COLECO_ACT_KEYPAD + 1), 'q', NULL, 0);
    colecosession_key(sess, 'q', 1);
    same("Q on keypad 1", word(sess, 0), 0x7F71);
    colecosession_key(sess, 'q', 0);

    /* a release clears what the press asserted, after a rebind in between */
    colecosession_key(sess, KEY_UP, 1);
    colecosession_binding_set_key(sess, COLECO_TARGET_PORT(0, COLECO_ACT_UP), 'p', NULL, 0);
    same("Up held", word(sess, 0), 0x7E7F);
    colecosession_key(sess, KEY_UP, 0);
    same("released after the Up key was rebound", word(sess, 0), 0x7F7F);
    colecosession_bindings_reset(sess);

    /* auto-repeat is one press */
    colecosession_key(sess, KEY_UP, 1);
    colecosession_key(sess, KEY_UP, 1);
    colecosession_key(sess, KEY_UP, 1);
    colecosession_key(sess, KEY_UP, 0);
    same("three key-downs and one key-up leave it idle", word(sess, 0), 0x7F7F);

    check(colecosession_buttons_held(sess, 2) == 0 && colecosession_buttons_held(sess, -1) == 0,
          "buttons_held on a port that does not exist is 0");
    same("controller_word on one that does not exist is idle", word(sess, 5), 0x7F7F);
}

static void sysactions(colecosession *sess)
{
    int act = -1;

    printf("the console actions' queue:\n");
    check(colecosession_sysaction_take(sess, &act) == 0, "empty to begin with");
    colecosession_sysaction_post(sess, COLECO_SYSACT_RESET_CONFIG);
    colecosession_sysaction_post(sess, COLECO_SYSACT_RESET);
    colecosession_sysaction_post(sess, COLECO_SYSACT_RESET);
    check(colecosession_sysaction_take(sess, &act) == 1 && act == COLECO_SYSACT_RESET,
          "take returns the lowest pending action");
    check(colecosession_sysaction_take(sess, &act) == 1 && act == COLECO_SYSACT_RESET_CONFIG,
          "then the next");
    check(colecosession_sysaction_take(sess, &act) == 0, "two posts of one action were one");
    colecosession_sysaction_post(sess, 99);
    colecosession_sysaction_post(sess, -1);
    check(colecosession_sysaction_take(sess, NULL) == 0, "an action that does not exist is ignored");
}

int main(void)
{
    char cfg[512], data[512];
    colecosession_paths paths;
    colecosession *sess;

    encoder();

    test_tmpdir(cfg, sizeof cfg, "icfg");
    test_tmpdir(data, sizeof data, "idat");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";
    sess = colecosession_new(&paths);
    if (!sess) { fprintf(stderr, "colecosession_new failed\n"); return 1; }

    keyboard(sess);
    keypad(sess);
    pairs(sess);
    sources(sess);
    sysactions(sess);
    colecosession_free(sess);

    if (failures) {
        fprintf(stderr, "\ninput_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("\ninput_test: PASS\n");
    return 0;
}
