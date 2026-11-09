/*
 * KeyForward -- see KeyForward.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeyForward.h"

#include "colecosession.h"

bool colecoIsAppKey(const QKeyEvent *evt)
{
    const int qtkey = evt->key();
    return qtkey == Qt::Key_F9 || qtkey == Qt::Key_F12;
}

uint32_t colecoKeysymFromQt(const QKeyEvent *evt)
{
    /* On X11 and Wayland Qt reports the X keycode, which is the evdev code
     * plus 8. */
    const quint32 scan = evt->nativeScanCode();
    if (scan >= 8) {
        const uint32_t sym = colecosession_keysym_from_evdev(scan - 8);
        if (sym) return sym;
    }

    const int qtkey = evt->key();
    const bool keypad = (evt->modifiers() & Qt::KeypadModifier) != 0;
    switch (qtkey) {
    case Qt::Key_Up:      return COLECOSESSION_KEYSYM_UP;
    case Qt::Key_Down:    return COLECOSESSION_KEYSYM_DOWN;
    case Qt::Key_Left:    return COLECOSESSION_KEYSYM_LEFT;
    case Qt::Key_Right:   return COLECOSESSION_KEYSYM_RIGHT;
    case Qt::Key_Tab:     return COLECOSESSION_KEYSYM_TAB;
    case Qt::Key_Escape:  return COLECOSESSION_KEYSYM_ESCAPE;
    case Qt::Key_Return:  return COLECOSESSION_KEYSYM_RETURN;
    case Qt::Key_Enter:   return COLECOSESSION_KEYSYM_KP_ENTER;
    case Qt::Key_Backspace: return COLECOSESSION_KEYSYM_BACKSPACE;
    case Qt::Key_Space:   return COLECOSESSION_KEYSYM_SPACE;
    case Qt::Key_Shift:   return COLECOSESSION_KEYSYM_LSHIFT;
    case Qt::Key_Control: return COLECOSESSION_KEYSYM_LCTRL;
    case Qt::Key_Alt:     return COLECOSESSION_KEYSYM_LALT;
    case Qt::Key_Asterisk: if (keypad) return COLECOSESSION_KEYSYM_KP_MULTIPLY; break;
    case Qt::Key_Slash:    if (keypad) return COLECOSESSION_KEYSYM_KP_DIVIDE; break;
    default: break;
    }
    if (qtkey >= Qt::Key_F1 && qtkey <= Qt::Key_F12)
        return COLECOSESSION_KEYSYM_F1 + (uint32_t)(qtkey - Qt::Key_F1);
    if (qtkey >= Qt::Key_0 && qtkey <= Qt::Key_9) {
        const uint32_t digit = (uint32_t)(qtkey - Qt::Key_0);
        return keypad ? COLECOSESSION_KEYSYM_KP_0 + digit : (uint32_t)'0' + digit;
    }
    if (qtkey >= Qt::Key_A && qtkey <= Qt::Key_Z)
        return (uint32_t)('a' + (qtkey - Qt::Key_A));
    if (qtkey >= 0x20 && qtkey <= 0x7e)
        return (uint32_t)qtkey;
    return 0;
}
