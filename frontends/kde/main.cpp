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

    colecosession_start_opts opts;
    colecosession_default_opts(session, &opts);
    if (argc > 1) opts.cart_path = argv[1];

    if (colecosession_start(session, &opts) != 0) {
        /* Show the window anyway: the usual reason to be here is a fresh
         * install with no BIOS, and the window is where the import lives. */
        qWarning("%s", colecosession_last_error(session));
    }
    win.show();

    const int rc = app.exec();
    colecosession_stop(session);
    colecosession_free(session);
    return rc;
}
