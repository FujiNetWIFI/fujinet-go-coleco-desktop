/*
 * bindings_test -- the remappable binding table and the on-screen panel's
 * direct press path.
 *
 * Two things worth pinning that a screenshot cannot show:
 *
 *   Rebinding STEALS. A key must drive exactly one control. Letting one
 *   keystroke do two things is worse than losing the old binding, because
 *   the second effect is invisible until the moment it matters.
 *
 *   The panel presses controls DIRECTLY rather than synthesising keystrokes.
 *   Routing an on-screen button through the key table would mean the button
 *   stopped working the moment someone remapped the key it was pretending to
 *   be -- a bug that would look like the emulator ignoring the mouse.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "colecosession.h"

#define XK_Up 0xFF52

static int failures;

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok " : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    colecosession_paths paths;
    colecosession *s;
    char tmp[512], cfg[600], data[600], cmd[700];
    coleco_input_state st;
    int t_up0, t_kp5_0, t_fireL1;

    snprintf(tmp, sizeof tmp, "/tmp/coleco-bindings-test-%d", (int)getpid());
    snprintf(cfg, sizeof cfg, "%s/config", tmp);
    snprintf(data, sizeof data, "%s/data", tmp);

    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";

    s = colecosession_new(&paths);
    if (!s) { fprintf(stderr, "colecosession_new failed\n"); return 1; }

    t_up0    = COLECO_TARGET_PORT(0, COLECO_ACT_UP);
    t_kp5_0  = COLECO_TARGET_PORT(0, COLECO_ACT_KEYPAD + 5);
    t_fireL1 = COLECO_TARGET_PORT(1, COLECO_ACT_FIRE_L);

    printf("labels:\n");
    check("a keypad target names its port and key",
          strcmp(coleco_binding_label(t_kp5_0), "Port 1 5") == 0);
    check("a direction target reads sensibly",
          strcmp(coleco_binding_label(t_up0), "Port 1 Up") == 0);
    check("a system action names itself",
          strcmp(coleco_binding_label(COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET)),
                 "Reset Console") == 0);
    check("the default key name is shown",
          strcmp(coleco_binding_key_name(t_up0), "Up") == 0);
    check("...and for a digit", strcmp(coleco_binding_key_name(t_kp5_0), "5") == 0);

    printf("defaults drive the machine:\n");
    coleco_input_reset(&st);
    check("Up is bound", coleco_input_key(&st, XK_Up, 1) == 1);
    check("...and moves port 1 up", coleco_input_word(&st, 0) == 0x7E7F);
    coleco_input_key(&st, XK_Up, 0);

    printf("rebinding steals:\n");
    /* Give port 1's Up the key that currently drives port 1's keypad 5. */
    coleco_binding_set(s, t_up0, '5');
    coleco_input_reset(&st);
    coleco_input_key(&st, '5', 1);
    check("'5' now moves up, not keypad 5", coleco_input_word(&st, 0) == 0x7E7F);
    check("keypad 5 lost its key",
          coleco_binding_key_name(t_kp5_0)[0] == '\0');
    check("...and Up reports the new one",
          strcmp(coleco_binding_key_name(t_up0), "5") == 0);
    coleco_input_key(&st, '5', 0);
    check("the old Up key no longer does anything",
          coleco_input_key(&st, XK_Up, 1) == 0);

    printf("bindings persist:\n");
    colecosession_settings_flush(s);
    colecosession_free(s);
    s = colecosession_new(&paths);
    if (!s) { fprintf(stderr, "reopen failed\n"); return 1; }
    check("the remap survived a restart",
          strcmp(coleco_binding_key_name(t_up0), "5") == 0);
    check("...and so did the theft",
          coleco_binding_key_name(t_kp5_0)[0] == '\0');

    printf("defaults restore:\n");
    coleco_bindings_reset_defaults(s);
    check("Up is back to Up", strcmp(coleco_binding_key_name(t_up0), "Up") == 0);
    check("keypad 5 has its key again",
          strcmp(coleco_binding_key_name(t_kp5_0), "5") == 0);

    printf("the panel presses directly:\n");
    /* Remap something, then press it on the panel: the panel must be
     * unaffected, because it addresses the CONTROL, not the key. */
    coleco_binding_set(s, t_fireL1, 'q');
    coleco_input_reset(&st);
    coleco_input_apply(&st, 1, COLECO_ACT_FIRE_L, 1);
    check("the panel's Fire L works regardless of its binding",
          coleco_input_word(&st, 1) == 0x3F7F);
    coleco_input_apply(&st, 1, COLECO_ACT_FIRE_L, 0);
    check("...and releases", coleco_input_word(&st, 1) == 0x7F7F);

    coleco_input_apply(&st, 0, COLECO_ACT_KEYPAD + 11, 1);
    check("the panel's '#' key holds", coleco_input_word(&st, 0) == 0x7F7B);
    coleco_input_apply(&st, 0, COLECO_ACT_KEYPAD + 11, 0);

    printf("system actions are not controller state:\n");
    coleco_bindings_reset_defaults(s);
    check("F5 is a system action",
          coleco_input_key_sysaction(0xFFC2) == COLECO_SYSACT_RESET);
    check("F6 is the other one",
          coleco_input_key_sysaction(0xFFC3) == COLECO_SYSACT_RESET_CONFIG);
    check("a controller key is not a system action",
          coleco_input_key_sysaction(XK_Up) == -1);
    coleco_input_reset(&st);
    check("...and a system key leaves the controllers alone",
          coleco_input_key(&st, 0xFFC2, 1) == 1 &&
          coleco_input_word(&st, 0) == 0x7F7F);

    colecosession_free(s);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp);
    if (system(cmd) != 0) { /* best effort */ }

    if (failures) {
        fprintf(stderr, "\nbindings_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("\nbindings_test: PASS\n");
    return 0;
}
