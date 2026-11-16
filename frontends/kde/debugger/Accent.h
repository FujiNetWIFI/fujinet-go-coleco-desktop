/*
 * The app's accent as Qt colours, from the session header's macros: the
 * fill, the ink on it, and the accent as text or lines (the light variant on
 * a dark theme, where the fill itself is only ~1.5:1).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QColor>
#include <QPalette>
#include <QWidget>

extern "C" {
#include "colecosession.h"
}

inline QColor colecoDebugRgb(unsigned rgb)
{
    return QColor(int((rgb >> 16) & 0xFF), int((rgb >> 8) & 0xFF), int(rgb & 0xFF));
}

inline QColor colecoDebugAccentColor() { return colecoDebugRgb(COLECOSESSION_ACCENT_RGB); }
inline QColor colecoDebugAccentInk() { return colecoDebugRgb(COLECOSESSION_ACCENT_INK_RGB); }

/* The accent for text and lines: light on a dark theme, the fill otherwise. */
inline QColor colecoDebugAccentTextColor(const QWidget *widget)
{
    if (widget && widget->palette().color(QPalette::Window).lightness() < 128)
        return colecoDebugRgb(COLECOSESSION_ACCENT_LIGHT_RGB);
    return colecoDebugAccentColor();
}

/* A highlight fill and the ink on it: the icon blue with white ink on a light
 * theme; on a dark one the pure blue barely shows, so the light blue with
 * black ink. */
inline bool colecoDebugDarkTheme(const QWidget *widget)
{
    return widget && widget->palette().color(QPalette::Window).lightness() < 128;
}
inline QColor colecoDebugAccentFill(const QWidget *widget)
{
    return colecoDebugDarkTheme(widget) ? colecoDebugRgb(COLECOSESSION_ACCENT_LIGHT_RGB) : colecoDebugAccentColor();
}
inline QColor colecoDebugAccentFillInk(const QWidget *widget)
{
    return colecoDebugDarkTheme(widget) ? QColor(Qt::black) : colecoDebugAccentInk();
}
