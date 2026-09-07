/*
 * FujiNet Go ColecoVision -- the GNOME (GTK4/libadwaita) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <adwaita.h>
#include <stdlib.h>
#include <string.h>

#include "colecosession.h"
#include "window.h"

#define APP_ID "online.fujinet.go.coleco.gnome"

static colecosession *g_session;
static char *g_cart_arg;

static void on_activate(AdwApplication *app, gpointer user_data)
{
    colecosession_start_opts opts;
    GtkWidget *win;
    (void)user_data;

    /* One window per process: the machine, and the FujiNet cartridge inside
     * it, are process singletons (fujimail's port interface carries no
     * context). Raising the existing window is the honest response to a
     * second activation. */
    win = GTK_WIDGET(gtk_application_get_active_window(GTK_APPLICATION(app)));
    if (win) {
        gtk_window_present(GTK_WINDOW(win));
        return;
    }

    win = coleco_window_new(app, g_session);

    colecosession_default_opts(g_session, &opts);
    if (g_cart_arg)
        opts.cart_path = g_cart_arg;

    if (colecosession_start(g_session, &opts) != 0) {
        /* Show the window anyway: the usual reason to be here is a fresh
         * install with no BIOS, and the window is where the import lives. A
         * dialog over an empty desktop would be a dead end. */
        g_warning("%s", colecosession_last_error(g_session));
    }
    gtk_window_present(GTK_WINDOW(win));
}

static void on_open(GApplication *app, GFile **files, gint n_files,
                    const gchar *hint, gpointer user_data)
{
    (void)hint; (void)user_data;
    if (n_files > 0) {
        g_free(g_cart_arg);
        g_cart_arg = g_file_get_path(files[0]);
    }
    g_application_activate(app);
}

int main(int argc, char **argv)
{
    AdwApplication *app;
    int status;

    g_session = colecosession_new(NULL);
    if (!g_session) {
        g_printerr("Could not create the session (unusable config/data "
                   "directories?)\n");
        return 1;
    }

    app = adw_application_new(APP_ID, G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);

    status = g_application_run(G_APPLICATION(app), argc, argv);

    colecosession_stop(g_session);
    colecosession_free(g_session);
    g_object_unref(app);
    g_free(g_cart_arg);
    return status;
}
