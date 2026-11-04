/*
 * The app's accent colour (COLECOSESSION_ACCENT_RGB, the icon colour: the
 * TMS9918's dark blue, the pair of the ADAM app's light blue). Buttons in the
 * accent state -- Map while armed, the remap target -- carry the dynamic
 * property "accent" and wear it with white text; main.cpp's style sheet draws
 * them and sets the palette's selection colours to match.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QColor>
#include <QPushButton>
#include <QStyle>

#include "colecosession.h"

inline QColor colecoAccentColor()
{
    return QColor((COLECOSESSION_ACCENT_RGB >> 16) & 0xff,
                  (COLECOSESSION_ACCENT_RGB >> 8) & 0xff,
                  COLECOSESSION_ACCENT_RGB & 0xff);
}

/* Puts a button in or out of the accent state and repolishes it, so the
 * style sheet's [accent="true"] rule takes effect at once. */
inline void colecoSetAccent(QPushButton *b, bool on)
{
    if (b->property("accent").toBool() == on) return;
    b->setProperty("accent", on);
    b->style()->unpolish(b);
    b->style()->polish(b);
}
