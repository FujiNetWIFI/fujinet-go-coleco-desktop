/*
 * KeyForward -- see KeyForward.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeyForward.h"

uint32_t colecoKeysymFromQt(const QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_Up:      return 0xFF52;
    case Qt::Key_Down:    return 0xFF54;
    case Qt::Key_Left:    return 0xFF51;
    case Qt::Key_Right:   return 0xFF53;
    case Qt::Key_Tab:     return 0xFF09;
    case Qt::Key_Escape:  return 0xFF1B;
    case Qt::Key_Shift:   return 0xFFE1;
    case Qt::Key_Control: return 0xFFE3;
    case Qt::Key_Alt:     return 0xFFE9;
    case Qt::Key_F5:      return 0xFFC2;
    case Qt::Key_F6:      return 0xFFC3;
    case Qt::Key_F9:      return 0xFFC6;
    case Qt::Key_F12:     return 0xFFC9;
    case Qt::Key_Asterisk: return '*';
    case Qt::Key_NumberSign: return '#';
    default: break;
    }

    /* Digits and letters map straight across. Qt reports letters as their
     * uppercase code point; the binding table folds case, so passing the
     * uppercase form through is correct and needs no shift handling here. */
    const int k = e->key();
    if (k >= Qt::Key_0 && k <= Qt::Key_9)
        return (uint32_t)('0' + (k - Qt::Key_0));
    if (k >= Qt::Key_A && k <= Qt::Key_Z)
        return (uint32_t)('A' + (k - Qt::Key_A));
    return 0;
}
