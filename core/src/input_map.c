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
 * Which key drives which control is bindings.c's business, not this file's
 * -- every control is remappable. What stays here is the part that has to be
 * exactly right and is worth testing on its own: how a held-key set becomes
 * the active-low controller word the machine reads.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "colecosession.h"

/* bindings.c */
int coleco_binding_target_for_key(uint32_t keysym);

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

/* Apply one control, named the way the binding table names it. Shared by the
 * key handler and by the on-screen keypad window, which presses controls
 * directly rather than synthesising keystrokes. */
void coleco_input_apply(coleco_input_state *st, int port, int act, int down)
{
    if (port < 0 || port > 1) return;

    if (act >= COLECO_ACT_KEYPAD && act < COLECO_ACT_KEYPAD + COLECO_KEYPAD_KEYS) {
        keypad_set(st, port, act - COLECO_ACT_KEYPAD, down);
        return;
    }
    switch (act) {
    case COLECO_ACT_UP:     bit_set(&st->dir[port], DIR_UP, down); break;
    case COLECO_ACT_DOWN:   bit_set(&st->dir[port], DIR_DOWN, down); break;
    case COLECO_ACT_LEFT:   bit_set(&st->dir[port], DIR_LEFT, down); break;
    case COLECO_ACT_RIGHT:  bit_set(&st->dir[port], DIR_RIGHT, down); break;
    case COLECO_ACT_FIRE_L: bit_set(&st->fire[port], FIRE_L, down); break;
    case COLECO_ACT_FIRE_R: bit_set(&st->fire[port], FIRE_R, down); break;
    default: break;
    }
}

/* Returns the system action a key is bound to, or -1. The frontend handles
 * those itself -- they are not controller state. */
int coleco_input_key_sysaction(uint32_t keysym)
{
    int t = coleco_binding_target_for_key(keysym);
    if (t < 0 || t < COLECO_TARGET_SYSACT(0)) return -1;
    return t - COLECO_TARGET_SYSACT(0);
}

int coleco_input_key(coleco_input_state *st, uint32_t keysym, int down)
{
    int t = coleco_binding_target_for_key(keysym);
    if (t < 0) return 0;
    /* A system action is bound but is not controller state; the frontend
     * asks for it separately with coleco_input_key_sysaction. Report the key
     * as consumed either way, so it never falls through to the machine. */
    if (t >= COLECO_TARGET_SYSACT(0)) return 1;

    coleco_input_apply(st, t / COLECO_ACT_PER_PORT, t % COLECO_ACT_PER_PORT,
                       down);
    return 1;
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
