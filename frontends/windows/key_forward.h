/*
 * Win32 key messages to X11/xkb keysyms, by hardware scan code through the
 * session's HID table -- the same physical-key route the other frontends
 * take, so a binding names the key whatever Shift, NumLock or the layout is
 * doing -- and the rule every window of the app shares for handing a key to
 * the console.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdint.h>
#include <windows.h>

#include "colecosession.h"

static inline uint32_t coleco_keysym_from_msg(WPARAM vkey, LPARAM lparam)
{
    const unsigned scancode = (unsigned)((lparam >> 16) & 0xff);
    const int extended = (lparam >> 24) & 1;
    const uint32_t keysym = colecosession_keysym_from_win_scancode(scancode, extended);
    if (keysym) return keysym;
    /* Fallbacks for keys the scan-code table has no usage for. */
    switch (vkey) {
    case VK_UP: return COLECOSESSION_KEYSYM_UP;
    case VK_DOWN: return COLECOSESSION_KEYSYM_DOWN;
    case VK_LEFT: return COLECOSESSION_KEYSYM_LEFT;
    case VK_RIGHT: return COLECOSESSION_KEYSYM_RIGHT;
    case VK_ESCAPE: return COLECOSESSION_KEYSYM_ESCAPE;
    case VK_RETURN: return COLECOSESSION_KEYSYM_RETURN;
    case VK_SPACE: return COLECOSESSION_KEYSYM_SPACE;
    case VK_TAB: return COLECOSESSION_KEYSYM_TAB;
    case VK_BACK: return COLECOSESSION_KEYSYM_BACKSPACE;
    default: break;
    }
    if (vkey >= VK_F1 && vkey <= VK_F12)
        return COLECOSESSION_KEYSYM_F1 + (uint32_t)(vkey - VK_F1);
    if (vkey >= '0' && vkey <= '9') return (uint32_t)vkey;
    if (vkey >= 'A' && vkey <= 'Z') return (uint32_t)(vkey - 'A' + 'a');
    /* The numeric keypad keeps its own keysyms; the core falls back to the
     * digit row for a Keypad digit nothing is bound to. */
    if (vkey >= VK_NUMPAD0 && vkey <= VK_NUMPAD9)
        return COLECOSESSION_KEYSYM_KP_0 + (uint32_t)(vkey - VK_NUMPAD0);
    return 0;
}

/* The app's own keys, never the console's: F9 (Controllers), F12 (Debugger)
 * and Ctrl+H (Help is Ctrl+Shift+H, taken before it gets here). */
static inline int coleco_app_key(WPARAM vkey)
{
    if (vkey == VK_F9 || vkey == VK_F12) return 1;
    return (GetKeyState(VK_CONTROL) & 0x8000) && vkey == 'H';
}

/* A key press or release for the console from any of the app's windows: 1
 * when the console took it (it is bound). Windows reports Alt, and every key
 * pressed while Alt is down, as a system key; a bound one is the console's
 * and the rest (Alt+F4, the menu mnemonics, Alt+Esc) stay Windows'. An
 * auto-repeat of a bound key is swallowed without being pressed again. The
 * session's own actions (the Reset keys) are the caller's. */
static inline int coleco_forward_key(colecosession *sess, UINT msg, WPARAM vkey, LPARAM lparam)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const uint32_t keysym = coleco_keysym_from_msg(vkey, lparam);
    if (!keysym) return 0;
    if (!down)
        return colecosession_key(sess, keysym, 0) != 0;    /* always: a key let go after a shortcut must not stick */
    if (coleco_app_key(vkey) || (msg == WM_SYSKEYDOWN && vkey == VK_F4)) return 0;
    /* No Ctrl rule: Left Ctrl is Controller 1's Left Fire, so steering with
     * it held must reach the console. The menu accelerators are taken before
     * WM_KEYDOWN, and an unbound key is not the console's anyway. */
    if (lparam & (1 << 30)) return colecosession_target_for_key(sess, keysym) >= 0;
    return colecosession_key(sess, keysym, 1) != 0;
}
