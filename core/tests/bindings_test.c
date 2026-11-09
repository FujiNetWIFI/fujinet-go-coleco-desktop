/*
 * bindings_test -- the remappable table and the keyboard translator, pure:
 * the defaults (both hand controllers' twelve keypad keys, four directions
 * and two fire buttons, F5 and F6 for the resets), that no two targets share
 * a default key or a default pad button, the numeric-keypad fallback,
 * steal-and-describe, persistence through the settings store, names, the
 * native key code tables, and the migration of the 0.9.0 key-only format. No
 * machine is started.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "colecosession.h"
#include "test_tmpdir.h"

static int failures;
static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

static int key_target(colecosession *sess, uint32_t keysym)
{
    return colecosession_target_for_key(sess, keysym);
}

#define PAD1(act) COLECO_TARGET_PORT(0, act)
#define PAD2(act) COLECO_TARGET_PORT(1, act)
#define KEYPAD1(num) COLECO_TARGET_PORT(0, COLECO_ACT_KEYPAD + (num))
#define KEYPAD2(num) COLECO_TARGET_PORT(1, COLECO_ACT_KEYPAD + (num))

static void paths_for(colecosession_paths *paths, char *cfg, size_t cfgsz,
                      char *data, size_t datasz, const char *tag)
{
    char tagged[32];
    snprintf(tagged, sizeof tagged, "%scfg", tag);
    test_tmpdir(cfg, cfgsz, tagged);
    snprintf(tagged, sizeof tagged, "%sdat", tag);
    test_tmpdir(data, datasz, tagged);
    memset(paths, 0, sizeof *paths);
    paths->config_dir = cfg;
    paths->data_dir = data;
    paths->fujinet_lib = "";
}

/* A 0.9.0 settings file: the lines given, one per bind.N. */
static void write_old_settings(const char *cfg, const char *body)
{
    char path[640];
    FILE *file;
    snprintf(path, sizeof path, "%s/settings.ini", cfg);
    file = fopen(path, "w");
    if (!file) return;
    fputs(body, file);
    fclose(file);
}

static void defaults(void)
{
    char cfg[512], data[512], name[64];
    colecosession_paths paths;
    colecosession *sess;
    coleco_binding bnd;
    int tgt, other, keyclash = 0, padclash = 0, bad;

    paths_for(&paths, cfg, sizeof cfg, data, sizeof data, "bd");
    sess = colecosession_new(&paths);
    if (!sess) { failures++; return; }

    /* the table's shape */
    check(COLECO_TARGET_COUNT == 38, "38 targets in all");
    check(COLECO_ACT_PER_PORT == 18 && COLECO_PORTS == 2, "18 actions on each of two controllers");
    check(COLECO_TARGET_SYSACT(0) == 36 && COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET_CONFIG) == 37,
          "the two console actions are 36 and 37");
    check(COLECO_ACT_UP == 12 && COLECO_ACT_FIRE_L == 16 && COLECO_ACT_FIRE_R == 17,
          "keypad 0-11, then the directions and fire buttons");

    /* controller 1: the digit row dials, the numeric keypad's * and / are * and # */
    check(key_target(sess, '0') == KEYPAD1(0) && key_target(sess, '5') == KEYPAD1(5)
          && key_target(sess, '9') == KEYPAD1(9), "controller 1's keypad 0-9 is the digit row");
    check(key_target(sess, COLECOSESSION_KEYSYM_KP_MULTIPLY) == KEYPAD1(10)
          && key_target(sess, COLECOSESSION_KEYSYM_KP_DIVIDE) == KEYPAD1(11),
          "Keypad * and Keypad / are * and #");
    check(key_target(sess, COLECOSESSION_KEYSYM_UP) == PAD1(COLECO_ACT_UP)
          && key_target(sess, COLECOSESSION_KEYSYM_DOWN) == PAD1(COLECO_ACT_DOWN)
          && key_target(sess, COLECOSESSION_KEYSYM_LEFT) == PAD1(COLECO_ACT_LEFT)
          && key_target(sess, COLECOSESSION_KEYSYM_RIGHT) == PAD1(COLECO_ACT_RIGHT),
          "the arrow keys steer controller 1");
    check(key_target(sess, COLECOSESSION_KEYSYM_LCTRL) == PAD1(COLECO_ACT_FIRE_L)
          && key_target(sess, COLECOSESSION_KEYSYM_LALT) == PAD1(COLECO_ACT_FIRE_R),
          "Left Ctrl and Left Alt are its fire buttons");
    /* controller 2 */
    check(key_target(sess, 'w') == PAD2(COLECO_ACT_UP) && key_target(sess, 's') == PAD2(COLECO_ACT_DOWN)
          && key_target(sess, 'a') == PAD2(COLECO_ACT_LEFT) && key_target(sess, 'd') == PAD2(COLECO_ACT_RIGHT),
          "W A S D steer controller 2");
    check(key_target(sess, 'W') == PAD2(COLECO_ACT_UP), "lookup folds case");
    check(key_target(sess, COLECOSESSION_KEYSYM_LSHIFT) == PAD2(COLECO_ACT_FIRE_L)
          && key_target(sess, COLECOSESSION_KEYSYM_TAB) == PAD2(COLECO_ACT_FIRE_R),
          "Left Shift and Tab are controller 2's fire buttons");
    bad = 0;
    for (tgt = 0; tgt < 12; tgt++)
        if (colecosession_binding_get(sess, KEYPAD2(tgt)).keysym != 0) bad++;
    check(bad == 0, "controller 2 has no keypad keys: one keyboard has one set of digits");

    /* the console */
    bnd = colecosession_binding_get(sess, COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET));
    check(bnd.keysym == COLECOSESSION_KEYSYM_F5 && bnd.button == COLECO_PAD_BTN_NONE, "Reset Console is F5");
    bnd = colecosession_binding_get(sess, COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET_CONFIG));
    check(bnd.keysym == COLECOSESSION_KEYSYM_F6 && bnd.button == COLECO_PAD_BTN_NONE, "Reset to CONFIG is F6");
    check(colecosession_key_sysaction(sess, COLECOSESSION_KEYSYM_F5) == COLECO_SYSACT_RESET
          && colecosession_key_sysaction(sess, COLECOSESSION_KEYSYM_F6) == COLECO_SYSACT_RESET_CONFIG
          && colecosession_key_sysaction(sess, COLECOSESSION_KEYSYM_UP) == -1,
          "key_sysaction names the console actions and nothing else");
    check(key_target(sess, COLECOSESSION_KEYSYM_F9) == -1 && key_target(sess, COLECOSESSION_KEYSYM_F12) == -1,
          "F9 and F12 are the app's: no default uses them");

    /* every default is its own: no key drives two targets, no button two targets of one pad */
    for (tgt = 0; tgt < COLECO_TARGET_COUNT; tgt++) {
        coleco_binding bnd_a = colecosession_binding_get(sess, tgt);
        for (other = tgt + 1; other < COLECO_TARGET_COUNT; other++) {
            coleco_binding bnd_b = colecosession_binding_get(sess, other);
            if (bnd_a.keysym && bnd_a.keysym == bnd_b.keysym) {
                printf("  (%s and %s share a key)\n", coleco_target_name(tgt), coleco_target_name(other));
                keyclash++;
            }
            /* the same button on both controllers is the default; within a
             * controller and against the console actions it must not repeat */
            if (bnd_a.button != COLECO_PAD_BTN_NONE && bnd_a.button == bnd_b.button
                && !(tgt < COLECO_ACT_PER_PORT && other >= COLECO_ACT_PER_PORT
                     && other < 2 * COLECO_ACT_PER_PORT)) {
                printf("  (%s and %s share a button)\n", coleco_target_name(tgt), coleco_target_name(other));
                padclash++;
            }
        }
    }
    check(keyclash == 0, "no two default keys clash");
    check(padclash == 0, "no default gamepad button drives two things");

    /* gamepad defaults, the same on both controllers */
    for (tgt = 0; tgt < 2; tgt++) {
        check(colecosession_target_for_button(sess, tgt, COLECO_PAD_BTN_DPAD_UP) == COLECO_TARGET_PORT(tgt, COLECO_ACT_UP)
              && colecosession_target_for_button(sess, tgt, COLECO_PAD_BTN_DPAD_DOWN) == COLECO_TARGET_PORT(tgt, COLECO_ACT_DOWN)
              && colecosession_target_for_button(sess, tgt, COLECO_PAD_BTN_DPAD_LEFT) == COLECO_TARGET_PORT(tgt, COLECO_ACT_LEFT)
              && colecosession_target_for_button(sess, tgt, COLECO_PAD_BTN_DPAD_RIGHT) == COLECO_TARGET_PORT(tgt, COLECO_ACT_RIGHT),
              "the D-pad steers, on either controller");
        check(colecosession_target_for_button(sess, tgt, COLECO_PAD_BTN_SOUTH) == COLECO_TARGET_PORT(tgt, COLECO_ACT_FIRE_L)
              && colecosession_target_for_button(sess, tgt, COLECO_PAD_BTN_EAST) == COLECO_TARGET_PORT(tgt, COLECO_ACT_FIRE_R),
              "South is the left fire and East the right fire");
        check(colecosession_target_for_button(sess, tgt, COLECO_PAD_BTN_WEST) == COLECO_TARGET_PORT(tgt, COLECO_ACT_KEYPAD + 1)
              && colecosession_target_for_button(sess, tgt, COLECO_PAD_BTN_NORTH) == COLECO_TARGET_PORT(tgt, COLECO_ACT_KEYPAD + 2)
              && colecosession_target_for_button(sess, tgt, COLECO_PAD_BTN_LEFT_SHOULDER) == COLECO_TARGET_PORT(tgt, COLECO_ACT_KEYPAD + 10)
              && colecosession_target_for_button(sess, tgt, COLECO_PAD_BTN_RIGHT_SHOULDER) == COLECO_TARGET_PORT(tgt, COLECO_ACT_KEYPAD + 11),
              "West and North are keypad 1 and 2, the shoulders * and #");
    }
    check(colecosession_target_for_button(sess, 0, COLECO_PAD_BTN_GUIDE) == -1
          && colecosession_target_for_button(sess, 0, COLECO_PAD_BTN_START) == -1
          && colecosession_target_for_button(sess, 0, COLECO_PAD_BTN_LEFT_TRIGGER) == -1
          && colecosession_target_for_button(sess, 0, COLECO_PAD_BTN_NONE) == -1
          && colecosession_target_for_button(sess, 0, COLECO_PAD_BTN_RAW_BASE + 3) == -1,
          "Guide, Start, the triggers and raw buttons are nothing until mapped");

    /* the numeric keypad falls back to the digit row, and only then */
    check(key_target(sess, COLECOSESSION_KEYSYM_KP_5) == KEYPAD1(5)
          && key_target(sess, COLECOSESSION_KEYSYM_KP_0) == KEYPAD1(0),
          "Keypad 5 and 0 dial 5 and 0 when nothing is bound to them");
    colecosession_binding_set_key(sess, PAD1(COLECO_ACT_UP), COLECOSESSION_KEYSYM_KP_5, NULL, 0);
    check(key_target(sess, COLECOSESSION_KEYSYM_KP_5) == PAD1(COLECO_ACT_UP),
          "a bound Keypad 5 is exact: the fallback gives way");
    check(key_target(sess, '5') == KEYPAD1(5), "binding Keypad 5 stole nothing from the digit 5");
    colecosession_bindings_reset(sess);
    check(key_target(sess, COLECOSESSION_KEYSYM_KP_ENTER) == -1, "Keypad Enter is Return and nothing is on it");
    colecosession_binding_set_key(sess, KEYPAD1(1), COLECOSESSION_KEYSYM_KP_ENTER, NULL, 0);
    check(key_target(sess, COLECOSESSION_KEYSYM_RETURN) == KEYPAD1(1)
          && colecosession_binding_get(sess, KEYPAD1(1)).keysym == COLECOSESSION_KEYSYM_RETURN,
          "Keypad Enter folds to Return");
    colecosession_bindings_reset(sess);

    /* steal: one key, one target */
    {
        char stolen[96];
        colecosession_binding_set_key(sess, KEYPAD1(7), COLECOSESSION_KEYSYM_LEFT, stolen, sizeof stolen);
        check(strstr(stolen, "Controller 1: Left") != NULL, "rebinding Left reports the holder it displaced");
        check(colecosession_binding_get(sess, PAD1(COLECO_ACT_LEFT)).keysym == 0, "the old holder lost the key");
        check(key_target(sess, COLECOSESSION_KEYSYM_LEFT) == KEYPAD1(7), "the new holder has it");
        colecosession_binding_set_key(sess, KEYPAD1(7), '7', stolen, sizeof stolen);
        check(stolen[0] == '\0' && key_target(sess, '7') == KEYPAD1(7), "putting 7 back takes nothing from anyone");
        colecosession_binding_set_key(sess, PAD1(COLECO_ACT_LEFT), COLECOSESSION_KEYSYM_LEFT, NULL, 0);
        colecosession_binding_set_key(sess, KEYPAD1(8), 'w', stolen, sizeof stolen);
        check(strstr(stolen, "Controller 2: Up") != NULL, "a key is taken from the other controller too");

        /* a pad button is unique within its controller and the console actions */
        colecosession_bindings_reset(sess);
        colecosession_binding_set_button(sess, KEYPAD1(3), COLECO_PAD_BTN_SOUTH, stolen, sizeof stolen);
        check(strstr(stolen, "Controller 1: Left Fire") != NULL, "stealing South names the target that had it");
        check(colecosession_binding_get(sess, PAD1(COLECO_ACT_FIRE_L)).button == COLECO_PAD_BTN_NONE
              && colecosession_target_for_button(sess, 0, COLECO_PAD_BTN_SOUTH) == KEYPAD1(3),
              "which lost it");
        check(colecosession_binding_get(sess, PAD2(COLECO_ACT_FIRE_L)).button == COLECO_PAD_BTN_SOUTH
              && colecosession_target_for_button(sess, 1, COLECO_PAD_BTN_SOUTH) == PAD2(COLECO_ACT_FIRE_L),
              "but controller 2's left fire kept its own South");
        colecosession_binding_set_button(sess, COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET),
                                         COLECO_PAD_BTN_START, stolen, sizeof stolen);
        check(stolen[0] == '\0' && colecosession_target_for_button(sess, 1, COLECO_PAD_BTN_START)
                  == COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET)
              && colecosession_target_for_button(sess, 0, COLECO_PAD_BTN_START)
                  == COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET),
              "a pad button bound to Reset Console works on either controller");
        colecosession_binding_set_button(sess, PAD2(COLECO_ACT_FIRE_R), COLECO_PAD_BTN_START, stolen, sizeof stolen);
        check(strstr(stolen, "Reset Console") != NULL, "and a controller's target takes it from the console action");
        colecosession_binding_set_key(sess, KEYPAD1(4), 0, NULL, 0);
        check(colecosession_binding_get(sess, KEYPAD1(4)).keysym == 0, "a key can be unbound");
        colecosession_bindings_reset(sess);
    }

    /* names */
    colecosession_keysym_name(COLECOSESSION_KEYSYM_F12, name, sizeof name);
    check(strcmp(name, "F12") == 0, "F12 names itself");
    colecosession_keysym_name('w', name, sizeof name);
    check(strcmp(name, "W") == 0, "letters name upper-case");
    colecosession_keysym_name(COLECOSESSION_KEYSYM_KP_7, name, sizeof name);
    check(strcmp(name, "Keypad 7") == 0, "numeric keypad keys say so");
    colecosession_keysym_name(COLECOSESSION_KEYSYM_KP_DIVIDE, name, sizeof name);
    check(strcmp(name, "Keypad /") == 0, "Keypad /");
    colecosession_keysym_name(COLECOSESSION_KEYSYM_LCTRL, name, sizeof name);
    check(strcmp(name, "Left Ctrl") == 0, "Left Ctrl");
    colecosession_keysym_name(COLECOSESSION_KEYSYM_HID_BASE + 0x68, name, sizeof name);
    check(strcmp(name, "F13") == 0, "the HID band names its keys");
    colecosession_keysym_name(COLECOSESSION_KEYSYM_HID_BASE + 0x01, name, sizeof name);
    check(strcmp(name, "HID 0x01") == 0, "and a HID usage with no name still gets one");
    check(strcmp(coleco_pad_button_name(COLECO_PAD_BTN_DPAD_LEFT), "D-pad Left") == 0
          && strcmp(coleco_pad_button_name(COLECO_PAD_BTN_RAW_BASE + 3), "Button 3") == 0
          && strcmp(coleco_pad_button_name(COLECO_PAD_HAT_BASE + 1), "Hat 0 Up-Right") == 0
          && strcmp(coleco_pad_button_name(COLECO_PAD_BTN_NONE), "") == 0,
          "pad button names: named, raw and hat");
    check(strcmp(coleco_target_name(KEYPAD1(5)), "Controller 1: Keypad 5") == 0
          && strcmp(coleco_target_name(KEYPAD2(10)), "Controller 2: Keypad *") == 0
          && strcmp(coleco_target_name(PAD2(COLECO_ACT_FIRE_L)), "Controller 2: Left Fire") == 0
          && strcmp(coleco_target_name(PAD1(COLECO_ACT_UP)), "Controller 1: Up") == 0,
          "controller target names");
    check(strcmp(coleco_target_name(COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET)), "Reset Console") == 0
          && strcmp(coleco_target_name(COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET_CONFIG)), "Reset to CONFIG") == 0,
          "console action names");
    check(strcmp(coleco_target_short_name(KEYPAD1(5)), "Keypad 5") == 0
          && strcmp(coleco_target_short_name(KEYPAD2(11)), "Keypad #") == 0
          && strcmp(coleco_target_short_name(PAD2(COLECO_ACT_RIGHT)), "Right") == 0
          && strcmp(coleco_target_short_name(PAD1(COLECO_ACT_FIRE_R)), "Right Fire") == 0
          && strcmp(coleco_target_short_name(COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET)), "Reset Console") == 0,
          "short names");
    check(coleco_target_name(KEYPAD1(5)) != coleco_target_name(KEYPAD1(6))
          && strcmp(coleco_target_name(KEYPAD1(5)), "Controller 1: Keypad 5") == 0,
          "names are stable strings: two lookups do not share a buffer");
    bad = 0;
    for (tgt = 0; tgt < COLECO_TARGET_COUNT; tgt++)
        if (coleco_target_name(tgt)[0] == '\0' || coleco_target_short_name(tgt)[0] == '\0') bad++;
    check(bad == 0, "every target has a name");
    check(coleco_target_name(-1)[0] == '\0' && coleco_target_name(COLECO_TARGET_COUNT)[0] == '\0',
          "and out of range has none");

    colecosession_free(sess);
}

static void native_codes(void)
{
    /* evdev KEY_1 is 2, Windows scancode 0x02 is '1', macOS 0x12 is '1' */
    check(colecosession_keysym_from_evdev(2) == '1', "evdev KEY_1 -> '1'");
    check(colecosession_keysym_from_win_scancode(0x02, 0) == '1', "Windows scancode 02 -> '1'");
    check(colecosession_keysym_from_macos_keycode(0x12) == '1', "macOS keycode 0x12 -> '1'");
    check(colecosession_keysym_from_evdev(103) == COLECOSESSION_KEYSYM_UP, "evdev KEY_UP -> Up");
    check(colecosession_keysym_from_win_scancode(0x48, 1) == COLECOSESSION_KEYSYM_UP, "Windows E0 48 -> Up");
    check(colecosession_keysym_from_macos_keycode(0x7E) == COLECOSESSION_KEYSYM_UP, "macOS 0x7E -> Up");
    check(colecosession_keysym_from_evdev(17) == 'w', "evdev KEY_W -> 'w'");
    /* the numeric keypad is the numeric keypad whatever NumLock says: the
     * position names the key, so KP 8 is KP_8 and not the cursor-up keysym */
    check(colecosession_keysym_from_evdev(72) == COLECOSESSION_KEYSYM_KP_8, "evdev KEY_KP8 -> Keypad 8");
    check(colecosession_keysym_from_win_scancode(0x48, 0) == COLECOSESSION_KEYSYM_KP_8, "Windows 48 -> Keypad 8");
    check(colecosession_keysym_from_evdev(55) == COLECOSESSION_KEYSYM_KP_MULTIPLY, "evdev KEY_KPASTERISK -> Keypad *");
    check(colecosession_keysym_from_evdev(98) == COLECOSESSION_KEYSYM_KP_DIVIDE, "evdev KEY_KPSLASH -> Keypad /");
    check(colecosession_keysym_from_evdev(29) == COLECOSESSION_KEYSYM_LCTRL
          && colecosession_keysym_from_evdev(56) == COLECOSESSION_KEYSYM_LALT
          && colecosession_keysym_from_evdev(42) == COLECOSESSION_KEYSYM_LSHIFT
          && colecosession_keysym_from_evdev(15) == COLECOSESSION_KEYSYM_TAB,
          "the default fire keys on evdev");
    check(colecosession_keysym_from_macos_keycode(0x3B) == COLECOSESSION_KEYSYM_LCTRL
          && colecosession_keysym_from_macos_keycode(0x3A) == COLECOSESSION_KEYSYM_LALT
          && colecosession_keysym_from_macos_keycode(0x38) == COLECOSESSION_KEYSYM_LSHIFT,
          "and on the Mac");
    check(colecosession_keysym_from_evdev(0xFFFF) == 0, "an unknown code is 0");
    check(colecosession_keysym_from_evdev(183) == COLECOSESSION_KEYSYM_HID_BASE + 0x68,
          "a key with no keysym lands in the HID band");
}

static void persistence(void)
{
    char cfg[512], data[512], stolen[96];
    colecosession_paths paths;
    colecosession *sess;

    paths_for(&paths, cfg, sizeof cfg, data, sizeof data, "bp");
    sess = colecosession_new(&paths);
    if (!sess) { failures++; return; }
    check(colecosession_get_str(sess, "bindings", NULL) == NULL,
          "a fresh install has no bindings key (nothing differs from the defaults)");

    colecosession_binding_set_key(sess, PAD2(COLECO_ACT_UP), 'z', stolen, sizeof stolen);
    check(stolen[0] == '\0', "Z was free");
    colecosession_binding_set_key(sess, PAD1(COLECO_ACT_FIRE_L), 'w', stolen, sizeof stolen);
    check(stolen[0] == '\0', "W was free too: controller 2's Up let go of it");
    colecosession_binding_set_button(sess, KEYPAD1(9), COLECO_PAD_BTN_WEST, NULL, 0);
    {
        const char *packed = colecosession_get_str(sess, "bindings", "");
        check(strstr(packed, "30.k:122") != NULL, "the settings hold the diff: controller 2 Up on z (122)");
        check(strstr(packed, "9.b:2") != NULL, "and the pad button (West is 2)");
        check(strstr(packed, "17.k:") == NULL, "but not an untouched default");
    }
    colecosession_free(sess);

    sess = colecosession_new(&paths);
    if (!sess) { failures++; return; }
    check(key_target(sess, 'z') == PAD2(COLECO_ACT_UP), "the rebinding persisted");
    check(key_target(sess, 'w') == PAD1(COLECO_ACT_FIRE_L), "and so did the theft of W");
    check(colecosession_target_for_button(sess, 0, COLECO_PAD_BTN_WEST) == KEYPAD1(9)
          && colecosession_binding_get(sess, KEYPAD1(1)).button == COLECO_PAD_BTN_NONE,
          "and the pad button, which left keypad 1");
    colecosession_bindings_reset(sess);
    check(key_target(sess, 'w') == PAD2(COLECO_ACT_UP), "reset restores the defaults");
    check(colecosession_target_for_button(sess, 0, COLECO_PAD_BTN_WEST) == KEYPAD1(1), "all of them");
    check(strcmp(colecosession_get_str(sess, "bindings", "x"), "") == 0, "and persists nothing");
    colecosession_free(sess);

    /* the table is process-global and only a session's own settings refill
     * it: a new session on the same files starts from the defaults again */
    sess = colecosession_new(&paths);
    check(sess && key_target(sess, 'w') == PAD2(COLECO_ACT_UP), "a restart after the reset stays reset");
    colecosession_free(sess);
}

/* The 0.9.0 format: bind.<target>=<keysym>, absent = default, 0 = unbound. */
static void migration(void)
{
    char cfg[512], data[512];
    colecosession_paths paths;
    colecosession *sess;

    /* the old default for a target is skipped (Up is 0xFF52 = 65362; keypad 0 is 48) */
    paths_for(&paths, cfg, sizeof cfg, data, sizeof data, "m1");
    write_old_settings(cfg, "bind.12=65362\nbind.0=48\nbind.10=42\nbind.11=35\npalette=2\n");
    sess = colecosession_new(&paths);
    if (!sess) { failures++; return; }
    check(key_target(sess, COLECOSESSION_KEYSYM_UP) == PAD1(COLECO_ACT_UP)
          && key_target(sess, '0') == KEYPAD1(0), "a stored value equal to the old default is skipped");
    check(key_target(sess, COLECOSESSION_KEYSYM_KP_MULTIPLY) == KEYPAD1(10)
          && key_target(sess, COLECOSESSION_KEYSYM_KP_DIVIDE) == KEYPAD1(11)
          && key_target(sess, '*') == -1 && key_target(sess, '#') == -1,
          "the old * and # defaults move to Keypad * and Keypad /");
    check(colecosession_get_int(sess, "palette", 0) == 2, "other settings are untouched");
    check(colecosession_get_str(sess, "bindings", NULL) != NULL, "and the migration leaves its marker");
    colecosession_free(sess);

    /* a real change is kept; unbound is honoured; uppercase folds */
    paths_for(&paths, cfg, sizeof cfg, data, sizeof data, "m2");
    write_old_settings(cfg, "bind.12=119\nbind.16=0\nbind.5=69\nbind.4=65461\n");
    sess = colecosession_new(&paths);
    if (!sess) { failures++; return; }
    check(colecosession_binding_get(sess, PAD1(COLECO_ACT_UP)).keysym == 'w',
          "a customised key is kept (controller 1 Up on w)");
    check(key_target(sess, 'w') == PAD1(COLECO_ACT_UP)
          && colecosession_binding_get(sess, PAD2(COLECO_ACT_UP)).keysym == 0,
          "and it took the key from the default holder, controller 2's Up");
    check(colecosession_binding_get(sess, PAD1(COLECO_ACT_FIRE_L)).keysym == 0
          && key_target(sess, COLECOSESSION_KEYSYM_LCTRL) == -1, "a deliberately unbound target stays unbound");
    check(colecosession_binding_get(sess, KEYPAD1(5)).keysym == 'e'
          && key_target(sess, 'E') == KEYPAD1(5), "an uppercase E is folded to e");
    check(colecosession_binding_get(sess, KEYPAD1(4)).keysym == COLECOSESSION_KEYSYM_KP_5
          && key_target(sess, COLECOSESSION_KEYSYM_KP_5) == KEYPAD1(4),
          "a stored Keypad 5 stays exact");
    check(key_target(sess, '5') == -1, "and nothing is left on the digit 5, which keypad 5 gave up");
    colecosession_free(sess);

    /* a custom key equal to a new default clears that default's holder */
    paths_for(&paths, cfg, sizeof cfg, data, sizeof data, "m3");
    write_old_settings(cfg, "bind.5=65450\n");   /* keypad 5 on Keypad * (0xFFAA) */
    sess = colecosession_new(&paths);
    if (!sess) { failures++; return; }
    check(key_target(sess, COLECOSESSION_KEYSYM_KP_MULTIPLY) == KEYPAD1(5)
          && colecosession_binding_get(sess, KEYPAD1(10)).keysym == 0,
          "a migrated key beats the new default that held it");
    colecosession_free(sess);

    /* with "bindings" present the old keys are ignored, and a reset is not undone */
    paths_for(&paths, cfg, sizeof cfg, data, sizeof data, "m4");
    write_old_settings(cfg, "bind.12=119\nbindings=0.k:122\n");
    sess = colecosession_new(&paths);
    if (!sess) { failures++; return; }
    check(key_target(sess, 'z') == KEYPAD1(0) && colecosession_binding_get(sess, PAD1(COLECO_ACT_UP)).keysym
              == COLECOSESSION_KEYSYM_UP, "bindings wins: the old bind.N lines are inert");
    colecosession_bindings_reset(sess);
    colecosession_free(sess);
    sess = colecosession_new(&paths);
    check(sess && key_target(sess, '0') == KEYPAD1(0)
          && colecosession_binding_get(sess, PAD1(COLECO_ACT_UP)).keysym == COLECOSESSION_KEYSYM_UP,
          "Defaults is not undone by stale bind.N lines");
    colecosession_free(sess);

    /* an install that pressed Defaults in 0.9.0 has all 38 stored: all skipped */
    {
        static const unsigned old_keys[COLECO_TARGET_COUNT] = {
            48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 42, 35,       /* controller 1 keypad: 0-9 * # */
            65362, 65364, 65361, 65363, 65507, 65513,             /* Up Down Left Right Ctrl_L Alt_L */
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,                   /* controller 2 has no keypad */
            119, 115, 97, 100, 65505, 65289,                      /* w s a d Shift_L Tab */
            65474, 65475,                                         /* F5 F6 */
        };
        char body[2048];
        size_t len = 0;
        int tgt;

        paths_for(&paths, cfg, sizeof cfg, data, sizeof data, "m5");
        for (tgt = 0; tgt < COLECO_TARGET_COUNT; tgt++)
            len += (size_t)snprintf(body + len, sizeof body - len, "bind.%d=%u\n", tgt, old_keys[tgt]);
        write_old_settings(cfg, body);
        sess = colecosession_new(&paths);
        if (!sess) { failures++; return; }
        check(key_target(sess, COLECOSESSION_KEYSYM_KP_MULTIPLY) == KEYPAD1(10)
              && key_target(sess, 'w') == PAD2(COLECO_ACT_UP)
              && key_target(sess, COLECOSESSION_KEYSYM_LALT) == PAD1(COLECO_ACT_FIRE_R)
              && key_target(sess, COLECOSESSION_KEYSYM_F5) == COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET),
              "a file that pressed Defaults (all 38 stored) migrates to exactly the new defaults");
        check(strcmp(colecosession_get_str(sess, "bindings", "x"), "") == 0, "with nothing differing");
        colecosession_free(sess);
    }
}

int main(void)
{
    defaults();
    native_codes();
    persistence();
    migration();

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
