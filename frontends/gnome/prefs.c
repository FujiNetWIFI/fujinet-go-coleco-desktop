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
#include "help/help_window.h"

/* The four TMS9928A palettes colecovid.c knows how to build. */
static const char *const palette_names[] = {"Default (TMS9928A)", "Palette 2",
                                            "Palette 3", "Palette 4", NULL};

typedef struct {
    ColecoWindow *window;
    colecosession *session;
    void (*restart)(ColecoWindow *window);
    gboolean dirty;
    AdwDialog *dialog;
    GtkWidget *rom_row, *rom_mark;      /* the OS7.rom row */
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

#define EXTERNAL_PLACEHOLDER "127.0.0.1:11502"

/* The address field: valid text (or none, meaning 127.0.0.1:11502) is saved
 * and marks the machine dirty; anything else is flagged and not saved. */
static void addr_changed(GtkEditable *entry, gpointer user_data)
{
    PrefsState *state = user_data;
    const char *text = gtk_editable_get_text(entry);
    char host[64];
    int port;

    if (colecosession_parse_hostport(text, host, sizeof host, &port) != 0) {
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
        return;
    }
    gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
    if (g_strcmp0(colecosession_get_str(state->session, "fujinet_external_addr", ""),
                  text) == 0)
        return;
    colecosession_set_str(state->session, "fujinet_external_addr", text);
    state->dirty = TRUE;
}

static void external_toggled(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    gtk_widget_set_sensitive(GTK_WIDGET(user_data),
                             adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
    (void)pspec;
}

/* ---- the ROMs page ------------------------------------------------------ */

static void refresh_roms(PrefsState *state)
{
    gboolean have = colecosession_bios_available(state->session) != 0;

    adw_action_row_set_subtitle(ADW_ACTION_ROW(state->rom_row),
                                have ? "Imported" : "Not imported");
    gtk_widget_remove_css_class(state->rom_mark, have ? "dim-label" : "success");
    gtk_widget_add_css_class(state->rom_mark, have ? "success" : "dim-label");
    gtk_image_set_from_icon_name(GTK_IMAGE(state->rom_mark),
                                 have ? "emblem-ok-symbolic" : "action-unavailable-symbolic");
}

/* An import from Preferences does not power-cycle at once: closing the dialog
 * does, with everything else changed here. */
static void on_import_done(int imported, gpointer user_data)
{
    AdwDialog *dialog = user_data;
    /* Preferences may have closed while the file chooser was up */
    PrefsState *state = g_object_get_data(G_OBJECT(dialog), "prefs-state");
    if (state) {
        if (imported > 0)
            state->dirty = TRUE;
        refresh_roms(state);
    }
    g_object_unref(dialog);
}

static void on_import_clicked(GtkButton *b, gpointer user_data)
{
    PrefsState *state = user_data;
    const gboolean folder = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "coleco-folder")) != 0;
    coleco_window_import_bios(state->window, GTK_WIDGET(state->dialog), folder, FALSE,
                              on_import_done, g_object_ref(state->dialog));
}

static void on_open_roms(GtkButton *b, gpointer user_data)
{
    PrefsState *state = user_data;
    g_autoptr(GFile) dir = g_file_new_for_path(colecosession_roms_path(state->session));
    GtkFileLauncher *l = gtk_file_launcher_new(dir);
    (void)b;
    gtk_file_launcher_launch(l, GTK_WINDOW(state->window), NULL, NULL, NULL);
    g_object_unref(l);
}

static GtkWidget *info_row(const char *title, const char *subtitle)
{
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    return row;
}

static AdwPreferencesPage *build_roms_page(PrefsState *state)
{
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    GtkWidget *box, *files, *folder, *row, *open, *detail;

    adw_preferences_page_set_title(page, "ROMs");
    adw_preferences_page_set_name(page, "ROMs");
    adw_preferences_page_set_icon_name(page, "media-flash-symbolic");

    adw_preferences_group_set_title(group, "Import");
    adw_preferences_group_set_header_suffix(group, coleco_help_button("prefs/ROMs"));
    row = info_row("Import BIOS",
        "Your own copy: a single file, a zip (MAME's coleco.zip works) or a "
        "folder. The BIOS is recognised by its size and checksum, whatever it "
        "is called.");
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    files = gtk_button_new_with_label("Import BIOS\xe2\x80\xa6");
    folder = gtk_button_new_with_label("Import from a Folder\xe2\x80\xa6");
    gtk_widget_add_css_class(files, "coleco-accent");
    g_object_set_data(G_OBJECT(folder), "coleco-folder", GINT_TO_POINTER(1));
    g_signal_connect(files, "clicked", G_CALLBACK(on_import_clicked), state);
    g_signal_connect(folder, "clicked", G_CALLBACK(on_import_clicked), state);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), files);
    gtk_box_append(GTK_BOX(box), folder);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), box);
    adw_preferences_group_add(group, row);

    row = info_row("ROM folder", colecosession_roms_path(state->session));
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
    open = gtk_button_new_with_label("Open");
    gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
    g_signal_connect(open, "clicked", G_CALLBACK(on_open_roms), state);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), open);
    adw_preferences_group_add(group, row);
    adw_preferences_page_add(page, group);

    group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "Console");
    row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                  "ColecoVision BIOS (OS7.rom)");
    detail = gtk_label_new("8192 bytes   CRC-32 3AA93EF3");
    gtk_widget_add_css_class(detail, "dim-label");
    gtk_widget_add_css_class(detail, "caption");
    gtk_widget_set_valign(detail, GTK_ALIGN_CENTER);
    state->rom_mark = gtk_image_new();
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), state->rom_mark);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), detail);
    adw_preferences_group_add(group, row);
    adw_preferences_page_add(page, group);
    state->rom_row = row;
    refresh_roms(state);
    return page;
}

static void prefs_closed(AdwDialog *dialog, gpointer user_data)
{
    PrefsState *state = user_data;
    g_object_set_data(G_OBJECT(dialog), "prefs-state", NULL);
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
    GtkWidget *external, *addr;

    state->window = parent;
    state->session = session;
    state->restart = restart;

    adw_dialog_set_title(ADW_DIALOG(dialog), "Preferences");
    adw_preferences_page_set_title(page, "Machine");
    adw_preferences_page_set_name(page, "Machine");
    adw_preferences_page_set_icon_name(page, "applications-games-symbolic");

    adw_preferences_group_set_title(machine, "Machine");
    adw_preferences_group_set_header_suffix(machine, coleco_help_button("prefs/Machine"));
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
    external = switch_row(state, "Use external FujiNet",
                          "Skip the built-in FujiNet and dial another "
                          "instance instead. Set that instance's BoIP "
                          "listener to this address.",
                          "fujinet_external", 0);
    addr = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(addr),
                                  "Address:port, e.g. " EXTERNAL_PLACEHOLDER);
    gtk_editable_set_text(GTK_EDITABLE(addr),
                          colecosession_get_str(session, "fujinet_external_addr", ""));
    g_signal_connect(addr, "changed", G_CALLBACK(addr_changed), state);
    gtk_widget_set_sensitive(addr, adw_switch_row_get_active(ADW_SWITCH_ROW(external)));
    g_signal_connect(external, "notify::active", G_CALLBACK(external_toggled), addr);
    adw_preferences_group_add(fuji, external);
    adw_preferences_group_add(fuji, addr);

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
    state->dialog = ADW_DIALOG(dialog);
    g_object_set_data(G_OBJECT(dialog), "prefs-state", state);
    adw_preferences_dialog_add(dialog, build_roms_page(state));
    g_signal_connect(dialog, "closed", G_CALLBACK(prefs_closed), state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}
