/*
 * FujiNetLogWindow -- see FujiNetLogWindow.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FujiNetLogWindow.h"

#include <QFont>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

void fujinet_log_show(QWidget *parent, colecosession *session)
{
    static QPointer<QWidget> win;
    if (win) {
        win->raise();
        win->activateWindow();
        return;
    }
    win = new QWidget(parent, Qt::Window);
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setWindowTitle(QStringLiteral("FujiNet Console Log"));
    win->resize(820, 560);

    auto *view = new QPlainTextEdit(win);
    view->setReadOnly(true);
    QFont mono = view->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    view->setFont(mono);

    auto *layout = new QVBoxLayout(win);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view);

    auto *timer = new QTimer(win);
    auto refresh = [view, session]() {
        /* Static because the ring is large and this runs once a second: a
         * 128K stack buffer in a UI callback is not worth the risk. */
        static char buf[128 * 1024];
        const int n = colecosession_fujinet_copy_log(session, buf, sizeof(buf));
        const bool atEnd = view->verticalScrollBar()->value() ==
                           view->verticalScrollBar()->maximum();
        view->setPlainText(n > 0 ? QString::fromUtf8(buf)
                                 : QStringLiteral("(no FujiNet output yet)"));
        /* Follow the tail only while the reader is already at the bottom;
         * scrolling up to read something must not be yanked away a second
         * later. */
        if (atEnd)
            view->verticalScrollBar()->setValue(
                view->verticalScrollBar()->maximum());
    };
    QObject::connect(timer, &QTimer::timeout, view, refresh);
    timer->start(1000);
    refresh();
    win->show();
}
