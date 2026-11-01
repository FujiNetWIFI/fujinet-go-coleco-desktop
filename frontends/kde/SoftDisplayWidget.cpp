/*
 * SoftDisplayWidget -- see SoftDisplayWidget.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SoftDisplayWidget.h"

#include "DisplayGeometry.h"
#include "FrameConvert.h"

#include <QEvent>
#include <QPainter>

SoftDisplayWidget::SoftDisplayWidget(colecosession *session, QWidget *parent)
    : QWidget(parent), m_session(session), m_feed(new FrameFeed(session, this)),
      m_image(COLECOSESSION_FB_WIDTH, COLECOSESSION_FB_HEIGHT, QImage::Format_RGB32)
{
    m_image.fill(Qt::black);
    m_fb.resize((size_t)COLECOSESSION_FB_WIDTH * COLECOSESSION_FB_HEIGHT);
    setMinimumSize(256, 212);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    connect(m_feed, &FrameFeed::frameReady, this, &SoftDisplayWidget::onFrame);
}

SoftDisplayWidget::~SoftDisplayWidget()
{
    m_feed->setActive(false);
}

void SoftDisplayWidget::setTvAspect(bool tv) { m_tvAspect = tv; update(); }
void SoftDisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }

void SoftDisplayWidget::updateActive()
{
    m_feed->setActive(isVisible() && window() && !window()->isMinimized());
}

void SoftDisplayWidget::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (window())
        window()->installEventFilter(this);
    updateActive();
}

void SoftDisplayWidget::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    if (window())
        window()->removeEventFilter(this);
    updateActive();
}

bool SoftDisplayWidget::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::WindowStateChange || e->type() == QEvent::Show ||
        e->type() == QEvent::Hide)
        updateActive();
    return QWidget::eventFilter(watched, e);
}

void SoftDisplayWidget::onFrame()
{
    if (!colecosession_copy_frame(m_session, m_fb.data(), &m_serial))
        return;
    rgb565ToXrgb(m_fb.data(), reinterpret_cast<quint32 *>(m_image.bits()), m_fb.size());
    update();
}

void SoftDisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);

    p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    p.drawImage(displayRect(QSizeF(size()), m_tvAspect), m_image);
}
