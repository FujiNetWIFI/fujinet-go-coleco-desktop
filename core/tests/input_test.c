/*
 * input_test -- the key-to-controller mapping, pinned to exact words.
 *
 * Exact values rather than "something changed", because the controller word
 * is active-low with the directions and the two fire buttons split across
 * both bytes, and a sign error there produces a machine that reads as though
 * every direction is held at once -- which looks like a broken emulator, not
 * a broken table.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>

#include "colecosession.h"

#define XK_Up        0xFF52
#define XK_Down      0xFF54
#define XK_Left      0xFF51
#define XK_Right     0xFF53
#define XK_Control_L 0xFFE3
#define XK_Alt_L     0xFFE9
#define XK_Shift_L   0xFFE1
#define XK_Tab       0xFF09
#define XK_KP_5      0xFFB5

static int failures;

static void eq(const char *what, unsigned got, unsigned want)
{
    if (got == want) {
        printf("  ok  %-38s 0x%04X\n", what, got);
    } else {
        printf("  FAIL %-38s got 0x%04X want 0x%04X\n", what, got, want);
        failures++;
    }
}

int main(void)
{
    coleco_input_state st;

    printf("the encoder:\n");
    eq("idle", coleco_controller_encode(0,0,0,0,0,0,-1), 0x7F7F);
    eq("up", coleco_controller_encode(1,0,0,0,0,0,-1), 0x7E7F);
    eq("right", coleco_controller_encode(0,0,0,1,0,0,-1), 0x7D7F);
    eq("down", coleco_controller_encode(0,1,0,0,0,0,-1), 0x7B7F);
    eq("left", coleco_controller_encode(0,0,1,0,0,0,-1), 0x777F);
    eq("left fire", coleco_controller_encode(0,0,0,0,1,0,-1), 0x3F7F);
    eq("right fire", coleco_controller_encode(0,0,0,0,0,1,-1), 0x7F3F);
    eq("keypad 0", coleco_controller_encode(0,0,0,0,0,0,0), 0x7F70);
    eq("keypad 5", coleco_controller_encode(0,0,0,0,0,0,5), 0x7F75);
    eq("keypad * (10)", coleco_controller_encode(0,0,0,0,0,0,10), 0x7F7A);
    eq("keypad # (11)", coleco_controller_encode(0,0,0,0,0,0,11), 0x7F7B);
    /* Out of range must read as no key, not wrap into a real one. */
    eq("keypad 12 is no key", coleco_controller_encode(0,0,0,0,0,0,12), 0x7F7F);
    eq("keypad -1 is no key", coleco_controller_encode(0,0,0,0,0,0,-1), 0x7F7F);
    eq("up + right fire together",
       coleco_controller_encode(1,0,0,0,0,1,-1), 0x7E3F);

    printf("the key table:\n");
    coleco_input_reset(&st);
    eq("reset is idle on port 1", coleco_input_word(&st, 0), 0x7F7F);
    eq("reset is idle on port 2", coleco_input_word(&st, 1), 0x7F7F);

    if (!coleco_input_key(&st, XK_Up, 1)) { printf("  FAIL Up unbound\n"); failures++; }
    eq("Up held on port 1", coleco_input_word(&st, 0), 0x7E7F);
    eq("...leaves port 2 idle", coleco_input_word(&st, 1), 0x7F7F);
    coleco_input_key(&st, XK_Up, 0);
    eq("Up released", coleco_input_word(&st, 0), 0x7F7F);

    coleco_input_key(&st, XK_Left, 1);
    coleco_input_key(&st, XK_Control_L, 1);
    eq("Left + Ctrl", coleco_input_word(&st, 0), 0x377F);
    coleco_input_key(&st, XK_Alt_L, 1);
    eq("Left + both fires", coleco_input_word(&st, 0), 0x373F);
    coleco_input_key(&st, XK_Left, 0);
    coleco_input_key(&st, XK_Control_L, 0);
    coleco_input_key(&st, XK_Alt_L, 0);

    coleco_input_key(&st, 'w', 1);
    coleco_input_key(&st, XK_Shift_L, 1);
    eq("W + Shift on port 2", coleco_input_word(&st, 1), 0x3E7F);
    eq("...leaves port 1 idle", coleco_input_word(&st, 0), 0x7F7F);
    coleco_input_key(&st, 'w', 0);
    coleco_input_key(&st, XK_Shift_L, 0);
    coleco_input_key(&st, XK_Tab, 1);
    eq("Tab is port 2's right fire", coleco_input_word(&st, 1), 0x7F3F);
    coleco_input_key(&st, XK_Tab, 0);

    printf("keypad hold semantics:\n");
    coleco_input_key(&st, '5', 1);
    eq("5 held", coleco_input_word(&st, 0), 0x7F75);
    /* Rolling from one digit onto another without releasing the first must
     * leave a key held, not none: a game polling once a frame would otherwise
     * see the gap and drop the press. */
    coleco_input_key(&st, '7', 1);
    eq("roll 5 -> 7", coleco_input_word(&st, 0), 0x7F77);
    coleco_input_key(&st, '5', 0);
    eq("releasing the OLD key does not clear 7",
       coleco_input_word(&st, 0), 0x7F77);
    coleco_input_key(&st, '7', 0);
    eq("releasing 7 clears it", coleco_input_word(&st, 0), 0x7F7F);

    coleco_input_key(&st, XK_KP_5, 1);
    eq("the numeric keypad works too", coleco_input_word(&st, 0), 0x7F75);
    coleco_input_key(&st, XK_KP_5, 0);

    if (coleco_input_key(&st, 0xFF1B /* Escape */, 1)) {
        printf("  FAIL Escape should be unbound\n");
        failures++;
    } else {
        printf("  ok  %-38s\n", "Escape is left to the frontend");
    }

    if (failures) {
        fprintf(stderr, "\ninput_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("\ninput_test: PASS\n");
    return 0;
}
