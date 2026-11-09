/*
 * The main application window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "colecosession.h"

#define COLECO_TYPE_WINDOW (coleco_window_get_type())
G_DECLARE_FINAL_TYPE(ColecoWindow, coleco_window, COLECO, WINDOW,
                     AdwApplicationWindow)

/* Installs the display-wide accent CSS once (COLECOSESSION_ACCENT_RGB, the
 * icon's colour, with white text): the class "coleco-accent" marks a button
 * as highlighted -- the Controllers window's Map, the primary button in
 * Preferences. Call before the first window is made. */
void coleco_css_install(void);

GtkWidget *coleco_window_new(AdwApplication *app, colecosession *session);

/* A toast over the display (the hot-plug events, console actions). */
void coleco_window_toast(ColecoWindow *self, const char *text);

/* The one keyboard path: the main window's own key handler and the
 * Controllers window's both end here, so the console, F9 (Controllers), F12
 * (Debugger) and the console actions behave the same whichever window has the
 * focus. `keycode` is the GDK hardware keycode. Returns whether the key was
 * consumed. */
gboolean coleco_window_key(ColecoWindow *self, guint keyval, guint keycode,
                           GdkModifierType state, gboolean down);

/* Runs a COLECO_SYSACT_* with its toast. UI thread. */
void coleco_window_run_sysaction(ColecoWindow *self, int sysact);

/* Lets go of every key the keyboard holds (focus loss). */
void coleco_window_release_keys(ColecoWindow *self);

/* The Help context the main window shows now: "main", or "prefs/<page>"
 * while Preferences is open. */
const char *coleco_window_help_context(ColecoWindow *self);

/* Import BIOS: a chooser for files (several, zips allowed) or one folder, then
 * a result dialog over `over` (the window when NULL). `apply` power-cycles the
 * console when the import supplied a BIOS that was missing. `done` gets the
 * number of items imported, or -1 if the chooser was cancelled. */
void coleco_window_import_bios(ColecoWindow *self, GtkWidget *over, gboolean folder,
                               gboolean apply, void (*done)(int imported, gpointer user_data),
                               gpointer user_data);

/* A dropped or command-line file: a BIOS dump is imported, a cartridge is run,
 * a disk image goes to FujiNet's SD folder. */
void coleco_window_load_media(ColecoWindow *self, const char *path);
