/*
 * The Preferences dialog: a programmatic AdwPreferencesDialog, no .ui file.
 *
 * Everything here is a machine option -- colecosession_default_opts() reads
 * these keys out of the settings store when the session starts, so none of
 * them can take effect mid-run. Each change persists immediately (so the KDE,
 * macOS and Windows frontends see the same configuration through the same
 * INI) and the dialog restarts the session once, on close, if anything
 * changed. Restarting per-keystroke would tear the machine down under the
 * user while they were still deciding.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include "window.h"

/* The four TMS9928A palettes colecovid.c knows how to build. */
static const char *const palette_names[] = {"Default (TMS9928A)", "Palette 2",
                                            "Palette 3", "Palette 4", NULL};

typedef struct {
    ColecoWindow *window;
    colecosession *session;
    void (*restart)(ColecoWindow *window);
    gboolean dirty;
} PrefsState;

typedef struct {
    PrefsState *state;
    const char *key;
    int def;
} RowBinding;

/* A real GClosureNotify rather than a cast g_free: the two signatures
 * genuinely differ (the notify takes the closure as well), and casting
 * between incompatible function types is undefined behaviour that
 * -Wcast-function-type is right to flag. */
static void binding_free(gpointer p, GClosure *closure)
{
    (void)closure;
    g_free(p);
}

static RowBinding *binding_new(PrefsState *state, const char *key, int def)
{
    RowBinding *b = g_new0(RowBinding, 1);
    b->state = state;
    b->key = key;
    b->def = def;
    return b;
}

static void combo_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    if (colecosession_get_int(b->state->session, b->key, b->def) == sel)
        return;
    colecosession_set_int(b->state->session, b->key, sel);
    b->state->dirty = TRUE;
}

static void switch_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int on = adw_switch_row_get_active(ADW_SWITCH_ROW(row)) ? 1 : 0;
    (void)pspec;
    if (colecosession_get_int(b->state->session, b->key, b->def) == on)
        return;
    colecosession_set_int(b->state->session, b->key, on);
    b->state->dirty = TRUE;
}

static GtkWidget *combo_row(PrefsState *state, const char *title,
                            const char *subtitle, const char *key, int def,
                            const char *const *names)
{
    GtkWidget *row = adw_combo_row_new();
    GtkStringList *model = gtk_string_list_new(names);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    adw_combo_row_set_selected(
        ADW_COMBO_ROW(row),
        (guint)colecosession_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::selected", G_CALLBACK(combo_changed),
                          binding_new(state, key, def), binding_free, 0);
    g_object_unref(model);
    return row;
}

static GtkWidget *switch_row(PrefsState *state, const char *title,
                             const char *subtitle, const char *key, int def)
{
    GtkWidget *row = adw_switch_row_new();

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row),
                              colecosession_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::active", G_CALLBACK(switch_changed),
                          binding_new(state, key, def), binding_free, 0);
    return row;
}

static void prefs_closed(AdwDialog *dialog, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)dialog;
    if (state->dirty && state->restart)
        state->restart(state->window);
    g_free(state);
}

void coleco_prefs_show(ColecoWindow *parent, colecosession *session,
                       void (*restart)(ColecoWindow *parent))
{
    PrefsState *state = g_new0(PrefsState, 1);
    AdwPreferencesDialog *dialog =
        ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    AdwPreferencesPage *page =
        ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *machine =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *fuji =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *host =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());

    state->window = parent;
    state->session = session;
    state->restart = restart;

    adw_dialog_set_title(ADW_DIALOG(dialog), "Preferences");
    adw_preferences_page_set_title(page, "Machine");
    adw_preferences_page_set_icon_name(page, "applications-games-symbolic");

    adw_preferences_group_set_title(machine, "Machine");
    adw_preferences_group_set_description(
        machine, "Applied by restarting the session when this dialog closes");
    adw_preferences_group_add(
        machine,
        switch_row(state, "Super Game Module",
                   "Fit the Opcode SGM: an AY-3-8910 and 24K of RAM. "
                   "Cartridges that do not use it are unaffected.",
                   "sgm", 1));
    adw_preferences_group_add(
        machine,
        combo_row(state, "Palette", "Which TMS9928A colour set to render with",
                  "palette", 0, palette_names));
    adw_preferences_group_add(
        machine,
        switch_row(state, "Swap controller buttons",
                   "Exchange the left and right fire buttons on both ports",
                   "swap_buttons", 0));

    adw_preferences_group_set_title(fuji, "FujiNet");
    adw_preferences_group_add(
        fuji,
        switch_row(state, "Enable FujiNet",
                   "Run the in-process FujiNet the cartridge dials into. "
                   "Off means no network and no CONFIG client.",
                   "enable_fujinet", 1));

    adw_preferences_group_set_title(host, "Host");
    adw_preferences_group_add(
        host, switch_row(state, "Audio", "Open the system audio device",
                         "enable_audio", 1));
    adw_preferences_group_add(
        host,
        switch_row(state, "Gamepads", "Poll USB/Bluetooth gamepads",
                   "enable_gamepad", 1));

    adw_preferences_page_add(page, machine);
    adw_preferences_page_add(page, fuji);
    adw_preferences_page_add(page, host);
    adw_preferences_dialog_add(dialog, page);
    g_signal_connect(dialog, "closed", G_CALLBACK(prefs_closed), state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}
