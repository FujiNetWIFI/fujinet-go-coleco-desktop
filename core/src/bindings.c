/*
 * bindings -- the remappable key/gamepad table, the keyboard translator,
 * and every name the bindings editor and the settings store need.
 *
 * One flat target table (colecosession.h's COLECO_TARGET_* indices): per
 * controller (1, then 2) the twelve keypad keys, four directions and two fire
 * buttons, then the app's own actions (Reset Console, Reset to CONFIG): 38 in
 * all. Each target holds at most one keysym and one gamepad button. Rebinding
 * STEALS -- a key drives exactly one target, because one keystroke doing two
 * things is worse than losing the old binding: the second effect is invisible
 * until it matters.
 *
 * Seeds its defaults LAZILY so the table is usable with no session behind
 * it: a unit test with no settings store gets the documented default map.
 * Persisted as one packed "bindings" key holding only the entries that
 * differ from the defaults ("<target>.k:<keysym>" / "<target>.b:<button>").
 * The 0.9.0 format (one "bind.<target>" key per target, a bare keysym) is
 * migrated once, the first time "bindings" is absent.
 *
 * Keysyms are X11's, folded to lower case before lookup so a binding made
 * with 'w' still fires while Shift is held for something else. The numeric
 * keypad's keys are bound apart from the digit row's, except that Keypad 0-9
 * fall back to the digit-row target when nothing is bound to them, so the
 * numeric keypad dials like the phone pad it is; its Enter is Enter.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hid_keys.h"
#include "session_internal.h"

typedef struct {
    uint32_t keysym;
    int button;
} slot;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static slot s_table[COLECO_TARGET_COUNT];
static slot s_defaults[COLECO_TARGET_COUNT];
static int s_seeded;
static struct colecosession *s_store;   /* whose settings hold "bindings" */

/* ---- defaults ------------------------------------------------------------- */

/* The defaults. Controller 1 gets the digit row (the keypad), the arrows and
 * Left Ctrl / Left Alt because that is where a one-player game looks; the
 * numeric keypad's * and / are the keypad's * and #, which Shift+8 / Shift+3
 * could not be (a key is named by its position). Controller 2 gets W A S D,
 * Left Shift and Tab and no keypad: one keyboard has one set of digits, the
 * Controllers window covers the second keypad. F5 and F6 are Reset Console and
 * Reset to CONFIG; F9 and F12 (the Controllers window and the debugger) stay
 * free of the machine.
 *
 * Gamepad, the same on both controllers: the D-pad steers (the left analog
 * stick does too: gamepad_sdl.c), South is the left fire, East the right fire,
 * West and North are keypad 1 and 2 (what most title screens ask for), the
 * shoulders are * and #. Nothing else is bound: Resets are not on a pad by
 * default (an accidental press restarts the game). */
static void compute_defaults_locked(void)
{
    static const struct { int act; uint32_t key; int button; } pad1[] = {
        { COLECO_ACT_UP,     COLECOSESSION_KEYSYM_UP,    COLECO_PAD_BTN_DPAD_UP },
        { COLECO_ACT_DOWN,   COLECOSESSION_KEYSYM_DOWN,  COLECO_PAD_BTN_DPAD_DOWN },
        { COLECO_ACT_LEFT,   COLECOSESSION_KEYSYM_LEFT,  COLECO_PAD_BTN_DPAD_LEFT },
        { COLECO_ACT_RIGHT,  COLECOSESSION_KEYSYM_RIGHT, COLECO_PAD_BTN_DPAD_RIGHT },
        { COLECO_ACT_FIRE_L, COLECOSESSION_KEYSYM_LCTRL, COLECO_PAD_BTN_SOUTH },
        { COLECO_ACT_FIRE_R, COLECOSESSION_KEYSYM_LALT,  COLECO_PAD_BTN_EAST },
    };
    static const uint32_t pad2_keys[] = {
        'w', 's', 'a', 'd', COLECOSESSION_KEYSYM_LSHIFT, COLECOSESSION_KEYSYM_TAB,
    };
    int tgt, idx, port;

    for (tgt = 0; tgt < COLECO_TARGET_COUNT; tgt++) {
        s_defaults[tgt].keysym = 0;
        s_defaults[tgt].button = COLECO_PAD_BTN_NONE;
    }

    for (idx = 0; idx < 6; idx++) {
        s_defaults[COLECO_TARGET_PORT(0, pad1[idx].act)].keysym = pad1[idx].key;
        s_defaults[COLECO_TARGET_PORT(1, pad1[idx].act)].keysym = pad2_keys[idx];
        for (port = 0; port < COLECO_PORTS; port++)
            s_defaults[COLECO_TARGET_PORT(port, pad1[idx].act)].button = pad1[idx].button;
    }
    for (idx = 0; idx <= 9; idx++)
        s_defaults[COLECO_TARGET_PORT(0, COLECO_ACT_KEYPAD + idx)].keysym = (uint32_t)('0' + idx);
    s_defaults[COLECO_TARGET_PORT(0, COLECO_ACT_KEYPAD + 10)].keysym = COLECOSESSION_KEYSYM_KP_MULTIPLY;
    s_defaults[COLECO_TARGET_PORT(0, COLECO_ACT_KEYPAD + 11)].keysym = COLECOSESSION_KEYSYM_KP_DIVIDE;

    for (port = 0; port < COLECO_PORTS; port++) {
        s_defaults[COLECO_TARGET_PORT(port, COLECO_ACT_KEYPAD + 1)].button = COLECO_PAD_BTN_WEST;
        s_defaults[COLECO_TARGET_PORT(port, COLECO_ACT_KEYPAD + 2)].button = COLECO_PAD_BTN_NORTH;
        s_defaults[COLECO_TARGET_PORT(port, COLECO_ACT_KEYPAD + 10)].button = COLECO_PAD_BTN_LEFT_SHOULDER;
        s_defaults[COLECO_TARGET_PORT(port, COLECO_ACT_KEYPAD + 11)].button = COLECO_PAD_BTN_RIGHT_SHOULDER;
    }

    s_defaults[COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET)].keysym = COLECOSESSION_KEYSYM_F5;
    s_defaults[COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET_CONFIG)].keysym = COLECOSESSION_KEYSYM_F6;
}

uint32_t bindings_fold_key(uint32_t keysym)
{
    if (keysym >= 'A' && keysym <= 'Z') return keysym + 32;
    if (keysym == COLECOSESSION_KEYSYM_KP_ENTER) return COLECOSESSION_KEYSYM_RETURN;
    return keysym;
}

/* ---- persistence ---------------------------------------------------------- */

static void apply_persisted_locked(const char *packed)
{
    const char *ptr = packed;
    while (ptr && *ptr) {
        int target = -1; char kind = 0; long value = 0;
        const char *end = strchr(ptr, ' ');
        if (sscanf(ptr, "%d.%c:%ld", &target, &kind, &value) == 3
            && target >= 0 && target < COLECO_TARGET_COUNT) {
            if (kind == 'k') s_table[target].keysym = (uint32_t)value;
            else if (kind == 'b') s_table[target].button = (int)value;
        }
        if (!end) break;
        ptr = end + 1;
    }
}

static void pack_locked(char *out, size_t outsz)
{
    size_t len = 0;
    int tgt;
    out[0] = '\0';
    for (tgt = 0; tgt < COLECO_TARGET_COUNT; tgt++) {
        if (s_table[tgt].keysym != s_defaults[tgt].keysym)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.k:%u",
                                    len ? " " : "", tgt, s_table[tgt].keysym);
        if (len >= outsz) break;
        if (s_table[tgt].button != s_defaults[tgt].button)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.b:%d",
                                    len ? " " : "", tgt, s_table[tgt].button);
        if (len >= outsz) break;
    }
}

/* Called with s_lock held: settings_mtx is taken under it, and the settings
 * store never calls back into the bindings. */
static void persist_locked(void)
{
    char packed[8192];
    if (!s_store) return;
    pack_locked(packed, sizeof packed);
    colecosession_set_str(s_store, "bindings", packed);
}

static void ensure_seeded_locked(void)
{
    if (s_seeded) return;
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
}

/* The 0.9.0 defaults, key by key, as that release stored them: a stored value
 * equal to it was never customised (or came from "Defaults", which stored all
 * 38), so it is skipped and the new default applies. '*' and '#' are the only
 * two that moved (to Keypad * and Keypad /). */
static uint32_t old_default(int target)
{
    static const uint32_t pad1_acts[6] = {
        0xFF52, 0xFF54, 0xFF51, 0xFF53, 0xFFE3, 0xFFE9,   /* Up Down Left Right Ctrl_L Alt_L */
    };
    static const uint32_t pad2_acts[6] = { 'w', 's', 'a', 'd', 0xFFE1, 0xFF09 };
    int port, act;

    if (target == COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET)) return 0xFFC2;         /* F5 */
    if (target == COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET_CONFIG)) return 0xFFC3;  /* F6 */
    port = target / COLECO_ACT_PER_PORT;
    act = target % COLECO_ACT_PER_PORT;
    if (act >= COLECO_ACT_UP)
        return (port ? pad2_acts : pad1_acts)[act - COLECO_ACT_UP];
    if (port) return 0;
    if (act <= 9) return (uint32_t)('0' + act);
    return act == 10 ? '*' : '#';
}

/* Moves a 0.9.0 install's customisations into the new table: each stored
 * "bind.N" that differs from that release's default. A customised key clears
 * the same key from any other target, as a rebinding would, so the user's
 * explicit choice beats a default that merely held it (and a later
 * customised target beats an earlier one). Returns 1 if any "bind.N" was
 * there at all. s_lock held. */
static int migrate_old_locked(struct colecosession *sess)
{
    int tgt, other, found = 0;

    for (tgt = 0; tgt < COLECO_TARGET_COUNT; tgt++) {
        char name[32];
        int stored;
        uint32_t keysym;
        snprintf(name, sizeof name, "bind.%d", tgt);
        stored = colecosession_get_int(sess, name, -1);
        if (stored < 0) continue;
        found = 1;
        if ((uint32_t)stored == old_default(tgt)) continue;
        keysym = bindings_fold_key((uint32_t)stored);
        s_table[tgt].keysym = keysym;
        if (keysym)
            for (other = 0; other < COLECO_TARGET_COUNT; other++)
                if (other != tgt && s_table[other].keysym == keysym)
                    s_table[other].keysym = 0;
    }
    return found;
}

void bindings_init(struct colecosession *sess)
{
    const char *packed;

    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    s_store = sess;
    if (sess) {
        packed = colecosession_get_str(sess, "bindings", NULL);
        if (packed)
            apply_persisted_locked(packed);
        else if (migrate_old_locked(sess))
            persist_locked();   /* "bindings" now exists: the migration is done */
    }
    pthread_mutex_unlock(&s_lock);
}

void bindings_shutdown(struct colecosession *sess)
{
    pthread_mutex_lock(&s_lock);
    if (s_store == sess) s_store = NULL;
    pthread_mutex_unlock(&s_lock);
}

/* ---- lookups -------------------------------------------------------------- */

/* The first target holding exactly `keysym`, or -1. */
static int target_exact_locked(uint32_t keysym)
{
    int tgt;
    if (!keysym) return -1;
    for (tgt = 0; tgt < COLECO_TARGET_COUNT; tgt++)
        if (s_table[tgt].keysym == keysym) return tgt;
    return -1;
}

/* The digit-row key a numeric-keypad digit falls back to, else 0. */
static uint32_t keypad_alias(uint32_t keysym)
{
    if (keysym >= COLECOSESSION_KEYSYM_KP_0 && keysym <= COLECOSESSION_KEYSYM_KP_9)
        return (uint32_t)('0' + (keysym - COLECOSESSION_KEYSYM_KP_0));
    return 0;
}

static int target_for_key_locked(uint32_t keysym)
{
    int tgt = target_exact_locked(keysym);
    if (tgt < 0 && keypad_alias(keysym))
        tgt = target_exact_locked(keypad_alias(keysym));
    return tgt;
}

int colecosession_target_for_key(colecosession *sess, uint32_t keysym)
{
    int tgt;
    (void)sess;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    tgt = target_for_key_locked(bindings_fold_key(keysym));
    pthread_mutex_unlock(&s_lock);
    return tgt;
}

int bindings_targets_for_key(uint32_t keysym, int *out, int max)
{
    int tgt, num = 0;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    for (tgt = 0; tgt < COLECO_TARGET_COUNT && num < max && keysym; tgt++)
        if (s_table[tgt].keysym == keysym) out[num++] = tgt;
    if (num == 0 && keypad_alias(keysym)) {
        tgt = target_exact_locked(keypad_alias(keysym));
        if (tgt >= 0 && max > 0) out[num++] = tgt;
    }
    pthread_mutex_unlock(&s_lock);
    return num;
}

/* Gamepad buttons are scoped to the pad's controller: the same button is
 * free to mean different things on different controllers, so only that
 * controller's targets and the machine-wide actions are searched. */
int colecosession_target_for_button(colecosession *sess, int port, int button)
{
    int tgt, found = -1;
    (void)sess;
    if (button == COLECO_PAD_BTN_NONE) return -1;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (port < 0 || port >= COLECO_PORTS) port = 0;
    for (tgt = 0; tgt < COLECO_ACT_PER_PORT; tgt++)
        if (s_table[COLECO_TARGET_PORT(port, tgt)].button == button) {
            found = COLECO_TARGET_PORT(port, tgt);
            break;
        }
    if (found < 0)
        for (tgt = COLECO_TARGET_SYSACT(0); tgt < COLECO_TARGET_COUNT; tgt++)
            if (s_table[tgt].button == button) { found = tgt; break; }
    pthread_mutex_unlock(&s_lock);
    return found;
}

coleco_binding colecosession_binding_get(colecosession *sess, int target)
{
    coleco_binding bind = { 0, COLECO_PAD_BTN_NONE };
    (void)sess;
    if (target < 0 || target >= COLECO_TARGET_COUNT) return bind;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    bind.keysym = s_table[target].keysym;
    bind.button = s_table[target].button;
    pthread_mutex_unlock(&s_lock);
    return bind;
}

/* A key may legitimately drive several targets (two controllers' controls can
 * be put on one key by hand, or by an old file). An explicit rebinding is the
 * user saying "this key does exactly this", so it displaces every holder --
 * and names them all, comma-separated, so nothing is lost silently. */
static void describe_stolen(const int *victims, int count, char *stolen,
                            int stolensz)
{
    int idx, len = 0;
    if (!stolen || stolensz <= 0) return;
    stolen[0] = '\0';
    for (idx = 0; idx < count && len < stolensz; idx++)
        len += snprintf(stolen + len, (size_t)(stolensz - len), "%s%s",
                        idx ? ", " : "", coleco_target_name(victims[idx]));
}

void colecosession_binding_set_key(colecosession *sess, int target,
                                   uint32_t keysym, char *stolen, int stolensz)
{
    int victims[COLECO_TARGET_COUNT], count = 0, tgt;
    (void)sess;
    if (stolen && stolensz > 0) stolen[0] = '\0';
    if (target < 0 || target >= COLECO_TARGET_COUNT) return;
    keysym = bindings_fold_key(keysym);
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (keysym) {
        for (tgt = 0; tgt < COLECO_TARGET_COUNT; tgt++)
            if (tgt != target && s_table[tgt].keysym == keysym) {
                s_table[tgt].keysym = 0;
                victims[count++] = tgt;
            }
    }
    s_table[target].keysym = keysym;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, count, stolen, stolensz);
}

void colecosession_binding_set_button(colecosession *sess, int target,
                                      int button, char *stolen, int stolensz)
{
    int victims[COLECO_TARGET_COUNT], count = 0, tgt;
    int port;
    (void)sess;
    if (stolen && stolensz > 0) stolen[0] = '\0';
    if (target < 0 || target >= COLECO_TARGET_COUNT) return;
    port = target / COLECO_ACT_PER_PORT;   /* COLECO_PORTS: the system actions */
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (button != COLECO_PAD_BTN_NONE) {
        for (tgt = 0; tgt < COLECO_TARGET_COUNT; tgt++) {
            int tport = tgt / COLECO_ACT_PER_PORT;
            /* steal only within the same scope: this controller, or the
             * machine-wide targets, which every pad reaches */
            if (tgt != target && s_table[tgt].button == button
                && (tport == port || tport >= COLECO_PORTS || port >= COLECO_PORTS)) {
                s_table[tgt].button = COLECO_PAD_BTN_NONE;
                victims[count++] = tgt;
            }
        }
    }
    s_table[target].button = button;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, count, stolen, stolensz);
}

void colecosession_bindings_reset(colecosession *sess)
{
    (void)sess;
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
}

/* ---- names ---------------------------------------------------------------- */

static char s_names[COLECO_TARGET_COUNT][40];
static char s_shorts[COLECO_TARGET_COUNT][24];
static pthread_once_t s_names_once = PTHREAD_ONCE_INIT;

static void build_names(void)
{
    static const char *const act_names[COLECO_ACT_PER_PORT - COLECO_ACT_UP] = {
        "Up", "Down", "Left", "Right", "Left Fire", "Right Fire",
    };
    int tgt;

    for (tgt = 0; tgt < COLECO_TARGET_COUNT; tgt++) {
        char *shrt = s_shorts[tgt];
        if (tgt >= COLECO_TARGET_SYSACT(0)) {
            snprintf(shrt, sizeof s_shorts[0], "%s",
                     tgt == COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET)
                         ? "Reset Console" : "Reset to CONFIG");
            snprintf(s_names[tgt], sizeof s_names[0], "%s", shrt);
        } else {
            const int act = tgt % COLECO_ACT_PER_PORT;
            if (act < COLECO_KEYPAD_KEYS)
                snprintf(shrt, sizeof s_shorts[0], "Keypad %s",
                         colecosession_keypad_label(act));
            else
                snprintf(shrt, sizeof s_shorts[0], "%s", act_names[act - COLECO_ACT_UP]);
            snprintf(s_names[tgt], sizeof s_names[0], "Controller %d: %s",
                     tgt / COLECO_ACT_PER_PORT + 1, shrt);
        }
    }
}

const char *coleco_target_name(int target)
{
    if (target < 0 || target >= COLECO_TARGET_COUNT) return "";
    pthread_once(&s_names_once, build_names);
    return s_names[target];
}

const char *coleco_target_short_name(int target)
{
    if (target < 0 || target >= COLECO_TARGET_COUNT) return "";
    pthread_once(&s_names_once, build_names);
    return s_shorts[target];
}

static const char *const pad_button_names[COLECO_PAD_BTN_COUNT] = {
    "A", "B", "X", "Y", "Back", "Guide", "Start", "Left Stick", "Right Stick",
    "Left Shoulder", "Right Shoulder", "D-pad Up", "D-pad Down", "D-pad Left",
    "D-pad Right", "Left Trigger", "Right Trigger",
};

const char *coleco_pad_button_name(int button)
{
    static char buf[32];
    if (COLECO_PAD_BTN_IS_NAMED(button)) return pad_button_names[button];
    if (COLECO_PAD_BTN_IS_RAW(button)) {
        snprintf(buf, sizeof buf, "Button %d", button - COLECO_PAD_BTN_RAW_BASE);
        return buf;
    }
    if (COLECO_PAD_BTN_IS_HAT(button)) {
        static const char *const dirs[COLECO_PAD_HAT_DIRS] =
            { "Up", "Up-Right", "Right", "Down-Right", "Down", "Down-Left",
              "Left", "Up-Left" };
        int hat = (button - COLECO_PAD_HAT_BASE) / COLECO_PAD_HAT_DIRS;
        int dir = (button - COLECO_PAD_HAT_BASE) % COLECO_PAD_HAT_DIRS;
        snprintf(buf, sizeof buf, "Hat %d %s", hat, dirs[dir]);
        return buf;
    }
    return "";
}

int colecosession_keysym_name(uint32_t keysym, char *dst, int dstsz)
{
    const char *name = NULL;
    if (!dst || dstsz <= 0) return 0;
    switch (keysym) {
    case 0: name = ""; break;
    case COLECOSESSION_KEYSYM_UP: name = "Up"; break;
    case COLECOSESSION_KEYSYM_DOWN: name = "Down"; break;
    case COLECOSESSION_KEYSYM_LEFT: name = "Left"; break;
    case COLECOSESSION_KEYSYM_RIGHT: name = "Right"; break;
    case COLECOSESSION_KEYSYM_ESCAPE: name = "Escape"; break;
    case COLECOSESSION_KEYSYM_RETURN: name = "Return"; break;
    case COLECOSESSION_KEYSYM_BACKSPACE: name = "Backspace"; break;
    case COLECOSESSION_KEYSYM_TAB: name = "Tab"; break;
    case COLECOSESSION_KEYSYM_SPACE: name = "Space"; break;
    case COLECOSESSION_KEYSYM_LSHIFT: name = "Left Shift"; break;
    case COLECOSESSION_KEYSYM_RSHIFT: name = "Right Shift"; break;
    case COLECOSESSION_KEYSYM_LCTRL: name = "Left Ctrl"; break;
    case COLECOSESSION_KEYSYM_RCTRL: name = "Right Ctrl"; break;
    case COLECOSESSION_KEYSYM_LALT: name = "Left Alt"; break;
    case COLECOSESSION_KEYSYM_RALT: name = "Right Alt"; break;
    case COLECOSESSION_KEYSYM_KP_ENTER: name = "Keypad Enter"; break;
    case COLECOSESSION_KEYSYM_KP_MULTIPLY: name = "Keypad *"; break;
    case COLECOSESSION_KEYSYM_KP_DIVIDE: name = "Keypad /"; break;
    case COLECOSESSION_KEYSYM_KP_PERIOD: name = "Keypad ."; break;
    case 0xffab: name = "Keypad +"; break;
    case 0xffad: name = "Keypad -"; break;
    case 0xffff: name = "Delete"; break;
    case 0xff63: name = "Insert"; break;
    case 0xff50: name = "Home"; break;
    case 0xff57: name = "End"; break;
    case 0xff55: name = "Page Up"; break;
    case 0xff56: name = "Page Down"; break;
    case 0xffe5: name = "Caps Lock"; break;
    default: break;
    }
    if (name) return snprintf(dst, (size_t)dstsz, "%s", name);
    if (keysym >= COLECOSESSION_KEYSYM_F1 && keysym <= COLECOSESSION_KEYSYM_F12)
        return snprintf(dst, (size_t)dstsz, "F%u", keysym - COLECOSESSION_KEYSYM_F1 + 1);
    if (keysym >= COLECOSESSION_KEYSYM_KP_0 && keysym <= COLECOSESSION_KEYSYM_KP_9)
        return snprintf(dst, (size_t)dstsz, "Keypad %u", keysym - COLECOSESSION_KEYSYM_KP_0);
    if (keysym >= 0x21 && keysym <= 0x7e) {
        char chr = (char)keysym;
        if (chr >= 'a' && chr <= 'z') chr = (char)(chr - 32);
        return snprintf(dst, (size_t)dstsz, "%c", chr);
    }
    if (keysym >= COLECOSESSION_KEYSYM_HID_BASE
        && keysym <= COLECOSESSION_KEYSYM_HID_BASE + COLECOSESSION_HID_USAGE_MAX) {
        const char *hid = coleco_hid_usage_name(keysym - COLECOSESSION_KEYSYM_HID_BASE);
        if (hid) return snprintf(dst, (size_t)dstsz, "%s", hid);
        return snprintf(dst, (size_t)dstsz, "HID 0x%02X",
                        (unsigned)(keysym - COLECOSESSION_KEYSYM_HID_BASE));
    }
    return snprintf(dst, (size_t)dstsz, "Key 0x%X", keysym);
}

/* ---- the keyboard translator --------------------------------------------- */

int colecosession_key_sysaction(colecosession *sess, uint32_t keysym)
{
    int tgt = colecosession_target_for_key(sess, keysym);
    if (tgt < COLECO_TARGET_SYSACT(0)) return -1;
    return tgt - COLECO_TARGET_SYSACT(0);
}

int colecosession_key(colecosession *sess, uint32_t keysym, int down)
{
    const uint32_t pressed = bindings_fold_key(keysym);
    int targets[COLECO_TARGET_COUNT], count, idx, hit = 0;

    if (!sess || !keysym) return 0;
    if (!down) {
        /* remembered by the key, so a release clears exactly what its press
         * asserted even if the binding changed in between */
        return input_key_release(sess, pressed);
    }
    /* every controller target the key drives; a system action is the
     * frontend's to run and is not a machine input */
    count = bindings_targets_for_key(pressed, targets, COLECO_TARGET_COUNT);
    for (idx = 0; idx < count; idx++) {
        if (targets[idx] >= COLECO_TARGET_SYSACT(0)) continue;
        hit = 1;
        input_key_hold(sess, targets[idx], pressed);
    }
    return hit;
}
