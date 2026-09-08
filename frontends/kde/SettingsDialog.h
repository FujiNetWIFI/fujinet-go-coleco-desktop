/*
 * SettingsDialog -- the machine options, all of them read by
 * colecosession_default_opts() at session start, so all of them need a
 * restart to apply. The caller does that restart when run() says something
 * changed; the display options (TV aspect, smooth scaling) apply live and
 * stay in the View menu instead.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COLECO_KDE_SETTINGSDIALOG_H
#define COLECO_KDE_SETTINGSDIALOG_H

#include <QDialog>

extern "C" {
#include "colecosession.h"
}

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    /* Returns true if a machine option changed, i.e. the caller should
     * restart the session. Returns false on Cancel, and also on OK when
     * nothing was actually altered -- restarting the machine because
     * somebody opened the dialog and pressed OK would be gratuitous. */
    static bool run(QWidget *parent, colecosession *session);
};

#endif
