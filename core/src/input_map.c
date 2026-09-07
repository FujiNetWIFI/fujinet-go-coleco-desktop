/*
 * input_map -- desktop key events to ColecoVision controller words.
 *
 * Two pure functions over an explicit held-key set, deliberately with no
 * session, no toolkit and no globals: this is the layer the sibling ports'
 * input bugs were actually caught in, and it is only testable in isolation if
 * it stays isolated. Frontends translate their toolkit's key events to
 * X11/xkb keysyms first (a GDK keyval already is one; Qt and Win32 map
 * through a small table), so one table serves all four.
 *
 * The ColecoVision has no keyboard. Everything is two hand controllers:
 * four directions, two fire buttons and a 12-key keypad each.
 *
 *   port 1   arrows           directions
 *            Left Ctrl        left fire
 *            Left Alt         right fire
 *            0-9 * #          keypad (top-row digits and the numpad)
 *
 *   port 2   W A S D          directions
 *            Left Shift       left fire
 *            Tab              right fire
 *            (keypad via the on-screen keypad window or a gamepad)
 *
 * Ctrl+digit is deliberately NOT the keypad here, unlike the ADAM app: that
 * app needs its digits for the ADAM's own keyboard, and this machine has no
 * keyboard to compete with.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "colecosession.h"

/* X11 keysyms, spelled out so this file needs no X headers. */
#define XK_Tab        0xFF09
#define XK_Escape     0xFF1B
#define XK_Left       0xFF51
#define XK_Up         0xFF52
#define XK_Right      0xFF53
#define XK_Down       0xFF54
#define XK_Shift_L    0xFFE1
#define XK_Control_L  0xFFE3
#define XK_Alt_L      0xFFE9
#define XK_KP_0       0xFFB0
#define XK_KP_9       0xFFB9
#define XK_KP_Mul     0xFFAA
#define XK_KP_Div     0xFFAF

/* Bit order matches the joystick-strobe byte adamcore reads. */
#define DIR_UP    0x01
#define DIR_RIGHT 0x02
#define DIR_DOWN  0x04
#define DIR_LEFT  0x08

#define FIRE_L 0x01
#define FIRE_R 0x02

void coleco_input_reset(coleco_input_state *st)
{
    memset(st, 0, sizeof *st);
    st->keypad[0] = -1;
    st->keypad[1] = -1;
}

/* A keypad key is HELD, not pulsed: the emulator samples the controller once
 * per frame, so a value present for only an instant falls between frames and
 * a polling game -- which is most of them, for skill select -- never sees it.
 * Releasing clears only if this key is the one still held, so rolling off one
 * digit onto another does not strand the machine with no key. */
static void keypad_set(coleco_input_state *st, int port, int key, int down)
{
    if (down)
        st->keypad[port] = (int8_t)key;
    else if (st->keypad[port] == key)
        st->keypad[port] = -1;
}

static void bit_set(uint8_t *field, uint8_t bit, int down)
{
    if (down) *field = (uint8_t)(*field | bit);
    else      *field = (uint8_t)(*field & ~bit);
}

int coleco_input_key(coleco_input_state *st, uint32_t keysym, int down)
{
    /* ---- port 1: arrows, Ctrl/Alt, digits ---- */
    switch (keysym) {
    case XK_Up:    bit_set(&st->dir[0], DIR_UP, down); return 1;
    case XK_Down:  bit_set(&st->dir[0], DIR_DOWN, down); return 1;
    case XK_Left:  bit_set(&st->dir[0], DIR_LEFT, down); return 1;
    case XK_Right: bit_set(&st->dir[0], DIR_RIGHT, down); return 1;
    case XK_Control_L: bit_set(&st->fire[0], FIRE_L, down); return 1;
    case XK_Alt_L:     bit_set(&st->fire[0], FIRE_R, down); return 1;

    /* ---- port 2: WASD, Shift/Tab ---- */
    case 'w': case 'W': bit_set(&st->dir[1], DIR_UP, down); return 1;
    case 's': case 'S': bit_set(&st->dir[1], DIR_DOWN, down); return 1;
    case 'a': case 'A': bit_set(&st->dir[1], DIR_LEFT, down); return 1;
    case 'd': case 'D': bit_set(&st->dir[1], DIR_RIGHT, down); return 1;
    case XK_Shift_L: bit_set(&st->fire[1], FIRE_L, down); return 1;
    case XK_Tab:     bit_set(&st->fire[1], FIRE_R, down); return 1;
    default: break;
    }

    /* ---- port 1 keypad: the top-row digits and the numeric keypad ---- */
    if (keysym >= '0' && keysym <= '9') {
        keypad_set(st, 0, (int)(keysym - '0'), down);
        return 1;
    }
    if (keysym >= XK_KP_0 && keysym <= XK_KP_9) {
        keypad_set(st, 0, (int)(keysym - XK_KP_0), down);
        return 1;
    }
    if (keysym == '*' || keysym == XK_KP_Mul) {
        keypad_set(st, 0, 10, down);
        return 1;
    }
    if (keysym == '#' || keysym == XK_KP_Div) {
        keypad_set(st, 0, 11, down);
        return 1;
    }
    return 0;
}

/* Active-low, idle 0x7F7F. The high byte is what the machine reads under the
 * joystick strobe (directions plus the left fire); the low byte is what it
 * reads under the keypad strobe (the encoded key plus the right fire). */
uint16_t coleco_controller_encode(int up, int down, int left, int right,
                                  int fire_left, int fire_right, int keypad)
{
    uint16_t s = 0x7F7F;

    if (up)    s &= (uint16_t)~0x0100;
    if (right) s &= (uint16_t)~0x0200;
    if (down)  s &= (uint16_t)~0x0400;
    if (left)  s &= (uint16_t)~0x0800;
    if (fire_left)  s &= (uint16_t)~0x4000;
    if (fire_right) s &= (uint16_t)~0x0040;

    /* Low nibble: the key index, or 0x0F for "no key". adamcore turns this
     * into the controller's wire encoding; values above 11 are not keys. */
    s = (uint16_t)((s & 0xFFF0) |
                   ((keypad >= 0 && keypad <= 11) ? (unsigned)keypad : 0x0F));
    return s;
}

uint16_t coleco_input_word(const coleco_input_state *st, int port)
{
    int p = port & 1;
    return coleco_controller_encode(st->dir[p] & DIR_UP,
                                    st->dir[p] & DIR_DOWN,
                                    st->dir[p] & DIR_LEFT,
                                    st->dir[p] & DIR_RIGHT,
                                    st->fire[p] & FIRE_L,
                                    st->fire[p] & FIRE_R,
                                    st->keypad[p]);
}
