/*
 * The FujiNet console log window: a live view of the in-process runtime's
 * captured output.
 *
 * This project is a protocol bring-up as much as an emulator -- the
 * cartridge dials into the FujiNet and every transaction is logged -- so
 * being able to watch that log without leaving the app is the difference
 * between "it does not work" and knowing which command failed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

extern "C" {
#include "colecosession.h"
}

/* Shows (raising an existing one) the FujiNet console log window. */
void fujinet_log_show(QWidget *parent, colecosession *session);
