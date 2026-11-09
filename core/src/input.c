/*
 * input -- every way a hand controller gets held, and the one word per
 * controller the machine sees.
 *
 * The inputs are held by sources (session_internal.h SRC_*: the keyboard, the
 * Controllers window's mouse, and for each gamepad its buttons and its stick),
 * one bit per source in an array per action, so letting go of one source
 * never releases another's. After every change the result is encoded and
 * handed to the host (coleco_host_joystick), which the machine samples once a
 * frame.
 *
 * A direction pair held both ways (UP and DOWN, LEFT and RIGHT) holds
 * neither, as a stick's switches cannot close both. The keypad strobe carries
 * ONE key (adamcore's keypad_code is an index), so when several are held the
 * lowest wins; colecosession_buttons_held reports exactly what the console
 * sees. Nothing here needs the machine to be running: with no machine the
 * push is a no-op and the held state stays readable, which is what lets the
 * tests run on a CI box with no BIOS.
 *
 * swap_buttons is NOT applied here: adamcore swaps the two fire buttons when
 * the machine reads the controller, so doing it again would put them back.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "host.h"
#include "session_internal.h"

#define IDLE_WORD 0x7F7F

void input_init(struct colecosession *sess)
{
    int port;
    for (port = 0; port < COLECO_PORTS; port++)
        sess->host_word[port] = IDLE_WORD;
}

/* ---- what the console sees ---------------------------------------------------- */

/* input_mtx held. */
static unsigned held_locked(const struct colecosession *sess, int port)
{
    unsigned mask = 0;
    int act;

    for (act = 0; act < COLECO_ACT_PER_PORT; act++)
        if (sess->pad_src[port][act])
            mask |= 1u << act;
    if ((mask & (1u << COLECO_ACT_UP)) && (mask & (1u << COLECO_ACT_DOWN)))
        mask &= ~((1u << COLECO_ACT_UP) | (1u << COLECO_ACT_DOWN));
    if ((mask & (1u << COLECO_ACT_LEFT)) && (mask & (1u << COLECO_ACT_RIGHT)))
        mask &= ~((1u << COLECO_ACT_LEFT) | (1u << COLECO_ACT_RIGHT));
    /* the keypad strobe holds one key: the lowest wins */
    for (act = 0; act < COLECO_KEYPAD_KEYS; act++)
        if (mask & (1u << act)) {
            mask &= ~((1u << COLECO_KEYPAD_KEYS) - 1u);
            mask |= 1u << act;
            break;
        }
    return mask;
}

static void push_port_locked(struct colecosession *sess, int port)
{
    const unsigned held = held_locked(sess, port);
    int keypad = -1, act;

    for (act = 0; act < COLECO_KEYPAD_KEYS; act++)
        if (held & (1u << act))
            keypad = act;
    sess->host_word[port] = coleco_controller_encode(
        (held >> COLECO_ACT_UP) & 1u, (held >> COLECO_ACT_DOWN) & 1u,
        (held >> COLECO_ACT_LEFT) & 1u, (held >> COLECO_ACT_RIGHT) & 1u,
        (held >> COLECO_ACT_FIRE_L) & 1u, (held >> COLECO_ACT_FIRE_R) & 1u,
        keypad);
    coleco_host_joystick(port, sess->host_word[port]);
}

static void push_all_locked(struct colecosession *sess)
{
    int port;
    for (port = 0; port < COLECO_PORTS; port++)
        push_port_locked(sess, port);
}

/* A start: nothing is held, and the machine is told. */
void input_clear(struct colecosession *sess)
{
    pthread_mutex_lock(&sess->input_mtx);
    memset(sess->pad_src, 0, sizeof sess->pad_src);
    memset(sess->held_keysym, 0, sizeof sess->held_keysym);
    push_all_locked(sess);
    pthread_mutex_unlock(&sess->input_mtx);
}

/* A power cycle makes a new machine, which starts idle: whatever is still
 * held (a key down across Reset to CONFIG, a pad's button) is handed to it. */
void input_after_power_cycle(struct colecosession *sess)
{
    pthread_mutex_lock(&sess->input_mtx);
    push_all_locked(sess);
    pthread_mutex_unlock(&sess->input_mtx);
}

static void set_src(uint32_t *word, uint32_t src, int down)
{
    if (down)
        *word |= src;
    else
        *word &= ~src;
}

/* One source of one controller target; system actions are not held inputs and
 * are left to the caller. input_mtx held. */
static void source_press_locked(struct colecosession *sess, int target,
                                uint32_t src, int down)
{
    const int port = target / COLECO_ACT_PER_PORT;
    set_src(&sess->pad_src[port][target % COLECO_ACT_PER_PORT], src, down);
    push_port_locked(sess, port);
}

/* ---- the public input API ------------------------------------------------------- */

void colecosession_press(colecosession *sess, int target, int down)
{
    if (!sess || target < 0 || target >= COLECO_TARGET_COUNT)
        return;
    if (target >= COLECO_TARGET_SYSACT(0)) {
        if (down)
            colecosession_sysaction(sess, target - COLECO_TARGET_SYSACT(0));
        return;
    }
    pthread_mutex_lock(&sess->input_mtx);
    source_press_locked(sess, target, SRC_KEYBOARD, down);
    pthread_mutex_unlock(&sess->input_mtx);
}

void input_key_hold(struct colecosession *sess, int target, uint32_t pressed)
{
    if (target < 0 || target >= COLECO_TARGET_SYSACT(0))
        return;
    pthread_mutex_lock(&sess->input_mtx);
    if (sess->held_keysym[target] != pressed) {   /* not an auto-repeat */
        sess->held_keysym[target] = pressed;
        source_press_locked(sess, target, SRC_KEYBOARD, 1);
    }
    pthread_mutex_unlock(&sess->input_mtx);
}

int input_key_release(struct colecosession *sess, uint32_t pressed)
{
    int target, count = 0;

    if (!pressed)          /* 0 is "nothing held" in held_keysym, not a key */
        return 0;
    pthread_mutex_lock(&sess->input_mtx);
    for (target = 0; target < COLECO_TARGET_SYSACT(0); target++)
        if (sess->held_keysym[target] == pressed) {
            sess->held_keysym[target] = 0;
            source_press_locked(sess, target, SRC_KEYBOARD, 0);
            count++;
        }
    pthread_mutex_unlock(&sess->input_mtx);
    return count;
}

void colecosession_sysaction_post(colecosession *sess, int sysact)
{
    if (sysact < 0 || sysact >= COLECO_SYSACT_COUNT)
        return;
    pthread_mutex_lock(&sess->sysact_mtx);
    sess->sysact_pending |= 1u << sysact;
    pthread_mutex_unlock(&sess->sysact_mtx);
}

int colecosession_sysaction_take(colecosession *sess, int *sysact)
{
    int found = 0, idx;

    pthread_mutex_lock(&sess->sysact_mtx);
    for (idx = 0; idx < COLECO_SYSACT_COUNT; idx++) {
        if (sess->sysact_pending & (1u << idx)) {
            sess->sysact_pending &= ~(1u << idx);
            if (sysact)
                *sysact = idx;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&sess->sysact_mtx);
    return found;
}

unsigned colecosession_buttons_held(colecosession *sess, int port)
{
    unsigned held;
    if (!sess || port < 0 || port >= COLECO_PORTS)
        return 0;
    pthread_mutex_lock(&sess->input_mtx);
    held = held_locked(sess, port);
    pthread_mutex_unlock(&sess->input_mtx);
    return held;
}

uint16_t colecosession_controller_word(colecosession *sess, int port)
{
    uint16_t word = IDLE_WORD;
    if (!sess || port < 0 || port >= COLECO_PORTS)
        return word;
    pthread_mutex_lock(&sess->input_mtx);
    word = sess->host_word[port];
    pthread_mutex_unlock(&sess->input_mtx);
    return word;
}

void colecosession_pointer_set(colecosession *sess, int port, unsigned mask)
{
    int act;
    if (!sess || port < 0 || port >= COLECO_PORTS)
        return;
    pthread_mutex_lock(&sess->input_mtx);
    for (act = 0; act < COLECO_ACT_PER_PORT; act++)
        set_src(&sess->pad_src[port][act], SRC_POINTER, (mask >> act) & 1u);
    push_port_locked(sess, port);
    pthread_mutex_unlock(&sess->input_mtx);
}

/* Everything the keyboard holds, let go (focus loss): what a key's press
 * asserted, whichever way it was asserted. A gamepad's own holds are left to
 * the gamepad, the mouse's to colecosession_pointer_set. */
void colecosession_release_all(colecosession *sess)
{
    int port, act;
    if (!sess)
        return;
    pthread_mutex_lock(&sess->input_mtx);
    memset(sess->held_keysym, 0, sizeof sess->held_keysym);
    for (port = 0; port < COLECO_PORTS; port++) {
        for (act = 0; act < COLECO_ACT_PER_PORT; act++)
            sess->pad_src[port][act] &= ~SRC_KEYBOARD;
        push_port_locked(sess, port);
    }
    pthread_mutex_unlock(&sess->input_mtx);
}

/* The gamepad thread's entry points: the same routing, from the pad's own
 * source bits, never touching the settings or the machine's state. */
void session_gamepad_press(struct colecosession *sess, int owner, int target, int down)
{
    if (owner < 0 || owner >= COLECO_MAX_PADS || target < 0
        || target >= COLECO_TARGET_SYSACT(0))
        return;
    pthread_mutex_lock(&sess->input_mtx);
    source_press_locked(sess, target, SRC_PAD(owner), down);
    pthread_mutex_unlock(&sess->input_mtx);
}

void session_gamepad_stick(struct colecosession *sess, int owner, int port, int act, int down)
{
    if (owner < 0 || owner >= COLECO_MAX_PADS || port < 0 || port >= COLECO_PORTS
        || act < 0 || act >= COLECO_ACT_PER_PORT)
        return;
    pthread_mutex_lock(&sess->input_mtx);
    source_press_locked(sess, COLECO_TARGET_PORT(port, act), SRC_STICK(owner), down);
    pthread_mutex_unlock(&sess->input_mtx);
}

/* ---- the encoder ----------------------------------------------------------------- */

/* Active-low, idle 0x7F7F. The high byte is what the machine reads under the
 * joystick strobe (directions plus the left fire); the low byte is what it
 * reads under the keypad strobe (the encoded key plus the right fire). */
uint16_t coleco_controller_encode(int upward, int down, int left, int right,
                                  int fire_left, int fire_right, int keypad)
{
    uint16_t word = IDLE_WORD;

    if (upward) word &= (uint16_t)~0x0100;
    if (right) word &= (uint16_t)~0x0200;
    if (down)  word &= (uint16_t)~0x0400;
    if (left)  word &= (uint16_t)~0x0800;
    if (fire_left)  word &= (uint16_t)~0x4000;
    if (fire_right) word &= (uint16_t)~0x0040;

    /* Low nibble: the key index, or 0x0F for "no key". adamcore turns this
     * into the controller's wire encoding; values above 11 are not keys. */
    word = (uint16_t)((word & 0xFFF0) |
                      ((keypad >= 0 && keypad <= 11) ? (unsigned)keypad : 0x0F));
    return word;
}
