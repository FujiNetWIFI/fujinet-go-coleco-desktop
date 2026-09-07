/*
 * The emulator display: a QWidget that pulls frames from the session.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QImage>
#include <QTimer>
#include <QWidget>
#include <cstdint>

#include "colecosession.h"

class DisplayWidget : public QWidget {
    Q_OBJECT
public:
    explicit DisplayWidget(colecosession *session, QWidget *parent = nullptr);
    void setTvAspect(bool tv);
    void setSmooth(bool smooth);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void tick();

    colecosession *m_session;
    QImage m_image;
    std::vector<uint16_t> m_fb;
    uint64_t m_serial = 0;   /* the session's type, not Qt's: quint64 is long long */
    QTimer m_timer;
    bool m_tv = true;
    bool m_smooth = false;
};
