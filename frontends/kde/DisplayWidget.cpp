/*
 * DisplayWidget -- see DisplayWidget.h.
 *
 * A QTimer rather than a frame-clock callback: Qt Widgets has no equivalent
 * of GdkFrameClock, and QOpenGLWidget's swap-interval trick buys a phase lock
 * at the cost of dragging a GL context into a 256x212 blit. The timer runs a
 * little faster than the machine so no frame waits a whole period, and the
 * session's own wall-clock pacing does the real work -- notify_vsync is still
 * fed, so the phase lock engages whenever the ticks happen to be steady.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include <QElapsedTimer>
#include <QPainter>

#include <chrono>

DisplayWidget::DisplayWidget(colecosession *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
    m_fb.resize(COLECOSESSION_FB_WIDTH * COLECOSESSION_FB_HEIGHT);
    m_image = QImage(COLECOSESSION_FB_WIDTH, COLECOSESSION_FB_HEIGHT,
                     QImage::Format_RGB32);
    m_image.fill(Qt::black);

    setMinimumSize(256, 212);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);

    connect(&m_timer, &QTimer::timeout, this, &DisplayWidget::tick);
    /* ~120 Hz: comfortably above the machine's 59.92 so a finished frame is
     * never held back by the poll interval. copy_frame does nothing when
     * there is no new frame, so the extra polls cost a mutex, not a copy. */
    m_timer.start(8);
}

void DisplayWidget::tick()
{
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    colecosession_notify_vsync(
        m_session,
        (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());

    if (!colecosession_copy_frame(m_session, m_fb.data(), &m_serial))
        return;

    /* RGB565 -> XRGB8888. The 5- and 6-bit fields are expanded by
     * replicating their high bits, not by shifting left: a plain shift maps
     * full-scale 0x1F to 0xF8, so white comes out slightly grey and every
     * colour a touch dark. */
    quint32 *out = reinterpret_cast<quint32 *>(m_image.bits());
    const int n = COLECOSESSION_FB_WIDTH * COLECOSESSION_FB_HEIGHT;
    for (int i = 0; i < n; i++) {
        const uint16_t p = m_fb[i];
        unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        out[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    update();
}

void DisplayWidget::setTvAspect(bool tv) { m_tv = tv; update(); }
void DisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }

void DisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);

    /* 256x212 is not 4:3: the 192 active lines are, and the 10-line borders
     * are part of the same 4:3 field on real hardware. So TV mode stretches
     * the whole buffer to 4:3 rather than letterboxing inside one. */
    const double want = m_tv ? (4.0 / 3.0)
                             : (double)COLECOSESSION_FB_WIDTH /
                               (double)COLECOSESSION_FB_HEIGHT;
    double w = width(), h = height(), sw, sh;
    if (w / h > want) { sh = h; sw = sh * want; }
    else              { sw = w; sh = sw / want; }

    p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    p.drawImage(QRectF((w - sw) / 2.0, (h - sh) / 2.0, sw, sh), m_image);
}
