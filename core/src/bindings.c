/*
 * bindings -- the remappable keyboard binding table.
 *
 * A flat array of targets (both controllers' controls, then the machine-wide
 * system actions) each holding at most one keysym, plus the reverse lookup
 * the key handler needs. Process-global, like the machine itself: there is
 * only ever one session live at a time, and threading a session pointer
 * through the key path would buy nothing.
 *
 * Rebinding STEALS: a key can drive exactly one control. Letting one
 * keystroke do two things is worse than losing the old binding, because the
 * second effect is invisible until it matters.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "session_internal.h"

/* X11 keysyms, spelled out so this file needs no X headers. */
#define XK_Tab        0xFF09
#define XK_Left       0xFF51
#define XK_Up         0xFF52
#define XK_Right      0xFF53
#define XK_Down       0xFF54
#define XK_F5         0xFFC2
#define XK_F6         0xFFC3
#define XK_Shift_L    0xFFE1
#define XK_Control_L  0xFFE3
#define XK_Alt_L      0xFFE9
#define XK_KP_Multiply 0xFFAA
#define XK_KP_Divide   0xFFAF
#define XK_KP_0        0xFFB0
#define XK_KP_9        0xFFB9

static uint32_t g_bind[COLECO_TARGET_COUNT];
static int g_seeded;

/* The defaults. Port 1 gets the arrows and the digits because that is where
 * a one-player game looks; port 2 gets WASD and no keypad, since a keyboard
 * has only one set of digits and the on-screen keypad window covers the
 * second controller's. */
static void seed_defaults(void)
{
    int i;
    memset(g_bind, 0, sizeof g_bind);
    g_seeded = 1;

    for (i = 0; i <= 9; i++)
        g_bind[COLECO_TARGET_PORT(0, COLECO_ACT_KEYPAD + i)] = (uint32_t)('0' + i);
    g_bind[COLECO_TARGET_PORT(0, COLECO_ACT_KEYPAD + 10)] = '*';
    g_bind[COLECO_TARGET_PORT(0, COLECO_ACT_KEYPAD + 11)] = '#';

    g_bind[COLECO_TARGET_PORT(0, COLECO_ACT_UP)] = XK_Up;
    g_bind[COLECO_TARGET_PORT(0, COLECO_ACT_DOWN)] = XK_Down;
    g_bind[COLECO_TARGET_PORT(0, COLECO_ACT_LEFT)] = XK_Left;
    g_bind[COLECO_TARGET_PORT(0, COLECO_ACT_RIGHT)] = XK_Right;
    g_bind[COLECO_TARGET_PORT(0, COLECO_ACT_FIRE_L)] = XK_Control_L;
    g_bind[COLECO_TARGET_PORT(0, COLECO_ACT_FIRE_R)] = XK_Alt_L;

    g_bind[COLECO_TARGET_PORT(1, COLECO_ACT_UP)] = 'w';
    g_bind[COLECO_TARGET_PORT(1, COLECO_ACT_DOWN)] = 's';
    g_bind[COLECO_TARGET_PORT(1, COLECO_ACT_LEFT)] = 'a';
    g_bind[COLECO_TARGET_PORT(1, COLECO_ACT_RIGHT)] = 'd';
    g_bind[COLECO_TARGET_PORT(1, COLECO_ACT_FIRE_L)] = XK_Shift_L;
    g_bind[COLECO_TARGET_PORT(1, COLECO_ACT_FIRE_R)] = XK_Tab;

    g_bind[COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET)] = XK_F5;
    g_bind[COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET_CONFIG)] = XK_F6;
}

/* ---- names ---------------------------------------------------------------- */

static const char *const keypad_label[COLECO_KEYPAD_KEYS] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "#"
};

const char *coleco_binding_label(int target)
{
    static char buf[48];
    int sa;

    if (target < 0 || target >= COLECO_TARGET_COUNT) return "";

    if (target >= COLECO_TARGET_SYSACT(0)) {
        sa = target - COLECO_TARGET_SYSACT(0);
        return sa == COLECO_SYSACT_RESET ? "Reset Console" : "Reset to CONFIG";
    }
    {
        int port = target / COLECO_ACT_PER_PORT;
        int act = target % COLECO_ACT_PER_PORT;
        const char *what;
        char keyname[8];

        if (act < COLECO_KEYPAD_KEYS) {
            snprintf(keyname, sizeof keyname, "%s", keypad_label[act]);
            what = keyname;
        } else {
            switch (act) {
            case COLECO_ACT_UP: what = "Up"; break;
            case COLECO_ACT_DOWN: what = "Down"; break;
            case COLECO_ACT_LEFT: what = "Left"; break;
            case COLECO_ACT_RIGHT: what = "Right"; break;
            case COLECO_ACT_FIRE_L: what = "Left Fire"; break;
            default: what = "Right Fire"; break;
            }
        }
        snprintf(buf, sizeof buf, "Port %d %s", port + 1, what);
    }
    return buf;
}

/* A short display name for a keysym. Enough for the Map row -- this is a
 * label, not a keymap. */
const char *coleco_binding_key_name(int target)
{
    static char buf[24];
    uint32_t k;

    if (!g_seeded) seed_defaults();
    if (target < 0 || target >= COLECO_TARGET_COUNT) return "";
    k = g_bind[target];
    if (!k) return "";

    switch (k) {
    case XK_Up: return "Up";
    case XK_Down: return "Down";
    case XK_Left: return "Left";
    case XK_Right: return "Right";
    case XK_Tab: return "Tab";
    case XK_Shift_L: return "Shift";
    case XK_Control_L: return "Ctrl";
    case XK_Alt_L: return "Alt";
    case XK_F5: return "F5";
    case XK_F6: return "F6";
    default: break;
    }
    if (k >= 0x20 && k < 0x7F) {
        buf[0] = (char)(k >= 'a' && k <= 'z' ? k - 32 : k);
        buf[1] = '\0';
        return buf;
    }
    snprintf(buf, sizeof buf, "0x%04X", k);
    return buf;
}

/* ---- the reverse lookup the key handler uses ------------------------------ */

/* Fold a keysym onto the one a binding is likely stored as.
 *
 * Case, so a binding made with 'w' still fires while Shift is held for
 * something else. And the numeric keypad onto the top row, so one binding
 * drives both -- a user who binds "5" means the digit, not one particular
 * key that produces it, and having the numpad silently do nothing on a
 * machine whose whole input model is a 12-key keypad would be perverse. */
static uint32_t fold(uint32_t k)
{
    if (k >= 'A' && k <= 'Z') return k + 32;
    if (k >= XK_KP_0 && k <= XK_KP_9) return (uint32_t)('0' + (k - XK_KP_0));
    if (k == XK_KP_Multiply) return '*';
    if (k == XK_KP_Divide) return '#';
    return k;
}

int coleco_binding_target_for_key(uint32_t keysym)
{
    int i;
    uint32_t folded;

    /* Usable with no session behind it. coleco_bindings_load() overlays the
     * user's own choices when there is one, but the defaults must be live
     * before that -- otherwise a frontend that forgot to load them, or a
     * unit test with no settings store, would silently have no input at
     * all rather than the documented default map. */
    if (!g_seeded) seed_defaults();
    folded = fold(keysym);

    for (i = 0; i < COLECO_TARGET_COUNT; i++) {
        if (!g_bind[i]) continue;
        if (g_bind[i] == keysym || g_bind[i] == folded) return i;
    }
    return -1;
}

/* ---- persistence ---------------------------------------------------------- */

static void key_for(int target, char *out, int n)
{
    snprintf(out, (size_t)n, "bind.%d", target);
}

void coleco_bindings_load(colecosession *s)
{
    int i;
    seed_defaults();
    for (i = 0; i < COLECO_TARGET_COUNT; i++) {
        char k[32];
        int v;
        key_for(i, k, sizeof k);
        /* -1 distinguishes "never stored" from "deliberately unbound" (0). */
        v = colecosession_get_int(s, k, -1);
        if (v >= 0) g_bind[i] = (uint32_t)v;
    }
}

static void store(colecosession *s, int target)
{
    char k[32];
    key_for(target, k, sizeof k);
    colecosession_set_int(s, k, (int)g_bind[target]);
}

void coleco_binding_set(colecosession *s, int target, uint32_t keysym)
{
    int i;
    if (target < 0 || target >= COLECO_TARGET_COUNT) return;

    if (keysym) {
        /* Steal it from whoever had it. */
        for (i = 0; i < COLECO_TARGET_COUNT; i++) {
            if (i != target && g_bind[i] == keysym) {
                g_bind[i] = 0;
                store(s, i);
            }
        }
    }
    g_bind[target] = keysym;
    store(s, target);
    colecosession_settings_flush(s);
}

void coleco_bindings_reset_defaults(colecosession *s)
{
    int i;
    seed_defaults();
    for (i = 0; i < COLECO_TARGET_COUNT; i++)
        store(s, i);
    colecosession_settings_flush(s);
}
