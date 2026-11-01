/*
 * Where the picture goes inside the display, shared by the GPU and the
 * software display.
 *
 * 256x212 is not 4:3: the 192 active lines are, and the 10-line borders are
 * part of the same 4:3 field on real hardware. So TV mode stretches the whole
 * buffer to 4:3 rather than letterboxing inside one; square pixels show the
 * raw 256:212.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QRectF>
#include <QSizeF>

#include "colecosession.h"

inline QRectF displayRect(QSizeF avail, bool tvAspect)
{
    const double want = tvAspect ? (4.0 / 3.0)
                                 : (double)COLECOSESSION_FB_WIDTH /
                                   (double)COLECOSESSION_FB_HEIGHT;
    const double w = avail.width(), h = avail.height();
    double sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }
    return QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh);
}
