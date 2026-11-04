/*
 * FujiNet Go ColecoVision -- the KDE (Qt6 Widgets) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>

#include "MainWindow.h"
#include "colecosession.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("fujinet-go-coleco-kde"));
    app.setApplicationDisplayName(QStringLiteral("FujiNet Go ColecoVision"));
    app.setDesktopFileName(QStringLiteral("online.fujinet.go.coleco.kde"));

    colecosession *session = colecosession_new(nullptr);
    if (!session) {
        qCritical("Could not create the session (unusable config/data dirs?)");
        return 1;
    }

    MainWindow win(session);

    win.show();
    /* A refusal for want of a BIOS (the usual state of a fresh install) opens
     * the import offer over the window, which is where the import lives. */
    win.start(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString());

    const int rc = app.exec();
    colecosession_stop(session);
    colecosession_free(session);
    return rc;
}
