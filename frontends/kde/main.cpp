/*
 * FujiNet Go ColecoVision -- the KDE (Qt6 Widgets) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QPalette>

#include "Accent.h"
#include "MainWindow.h"
#include "colecosession.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("fujinet-go-coleco-kde"));
    app.setApplicationDisplayName(QStringLiteral("FujiNet Go ColecoVision"));
    app.setDesktopFileName(QStringLiteral("online.fujinet.go.coleco.kde"));

    /* Armed/target buttons (Map, the remap target) and selections wear the
     * accent colour with white text, light or dark. The rule spells out the
     * whole box: with only a background colour the style still draws its own
     * bevel, which darkens it (Breeze), lightens it (Fusion) or ignores it
     * (Kvantum). */
    const QColor accent = colecoAccentColor();
    app.setStyleSheet(
        QStringLiteral(
            "QPushButton[accent=\"true\"] { background-color: %1; color: white;"
            " border: 1px solid %3; border-radius: 4px; padding: 4px 12px; }"
            "QPushButton[accent=\"true\"]:hover { background-color: %2; }")
            .arg(accent.name(), accent.lighter(112).name(),
                 accent.darker(140).name()));
    {
        QPalette pal = app.palette();
        pal.setColor(QPalette::Highlight, accent);
        pal.setColor(QPalette::HighlightedText, Qt::white);
        app.setPalette(pal);
    }

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
