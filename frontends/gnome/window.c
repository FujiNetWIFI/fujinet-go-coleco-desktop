/*
 * window.c -- the main window: the display, a header-bar menu, keyboard
 * capture, and the first-run BIOS gate.
 *
 * Keyboard events are translated to X11/xkb keysyms and handed to the
 * session's pure input table. A GDK keyval already IS an xkb keysym, so this
 * frontend passes them through unchanged -- the Qt and Win32 frontends map
 * through a small table to reach the same place, which is what lets one
 * tested table serve all four.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include "display.h"
#include "debugger/dbg_window.h"
#include "keypad/keypad_window.h"

#include <string.h>

struct _ColecoWindow {
    AdwApplicationWindow parent_instance;

    colecosession *session;
    GtkWidget *display;
    GtkWidget *toast_overlay;
    GtkWidget *status;          /* the FujiNet link indicator */
    coleco_input_state input;
    guint status_id;
};

G_DEFINE_FINAL_TYPE(ColecoWindow, coleco_window, ADW_TYPE_APPLICATION_WINDOW)

static void push_toast(ColecoWindow *self, const char *text)
{
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toast_overlay),
                                adw_toast_new(text));
}

/* ---- input ---------------------------------------------------------------- */

/* Push both ports every time. The controller word is a snapshot of everything
 * held, so sending only the port that changed would be an optimisation with a
 * bug in it the first time a key on each port is held at once. */
static void push_input(ColecoWindow *self)
{
    colecosession_joystick_raw(self->session, 0,
                               coleco_input_word(&self->input, 0));
    colecosession_joystick_raw(self->session, 1,
                               coleco_input_word(&self->input, 1));
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    ColecoWindow *self = user_data;
    int sysact;
    (void)ctrl; (void)keycode; (void)state;

    /* F9 belongs to the window, not the machine, and is deliberately not
     * bindable: it is how you reach the panel that does the binding. */
    if (keyval == GDK_KEY_F9) {
        coleco_keypad_window_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    }
    if (keyval == GDK_KEY_F12) {
        coleco_debugger_show(GTK_WINDOW(self), self->session);
        return TRUE;
    }
    sysact = coleco_input_key_sysaction(keyval);
    if (sysact >= 0) {
        colecosession_sysaction(self->session, sysact);
        return TRUE;
    }
    if (coleco_input_key(&self->input, keyval, 1)) {
        push_input(self);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_key_released(GtkEventControllerKey *ctrl, guint keyval,
                                guint keycode, GdkModifierType state,
                                gpointer user_data)
{
    ColecoWindow *self = user_data;
    (void)ctrl; (void)keycode; (void)state;

    if (keyval == GDK_KEY_F9 || keyval == GDK_KEY_F12) return TRUE;
    if (coleco_input_key_sysaction(keyval) >= 0) return TRUE;
    if (coleco_input_key(&self->input, keyval, 0)) {
        push_input(self);
        return TRUE;
    }
    return FALSE;
}

/* Losing focus with keys held would leave the machine believing they are
 * still down -- alt-tabbing mid-jump and coming back to a character walking
 * into a wall is the classic symptom. */
static void on_focus_leave(GtkEventControllerFocus *ctrl, gpointer user_data)
{
    ColecoWindow *self = user_data;
    (void)ctrl;
    coleco_input_reset(&self->input);
    push_input(self);
}

/* ---- status --------------------------------------------------------------- */

static gboolean update_status(gpointer user_data)
{
    ColecoWindow *self = user_data;
    const char *text;

    if (!colecosession_is_running(self->session))
        text = "Stopped";
    else if (!colecosession_cart_mailbox_live(self->session))
        text = "No FujiNet cartridge";
    else if (colecosession_cart_link_up(self->session))
        text = "FujiNet connected";
    else
        text = "FujiNet: link down";

    gtk_label_set_text(GTK_LABEL(self->status), text);
    return G_SOURCE_CONTINUE;
}

/* ---- actions -------------------------------------------------------------- */

/* Import a file and, if it is a cartridge, run it. Restarting is the honest
 * way to insert one: the cartridge device builds its window from the image
 * at construction, exactly as the RP2040 does at power-on. */
static void load_media(ColecoWindow *self, const char *path)
{
    char dest[1024];
    char msg[1200];

    if (colecosession_import_media(self->session, path, dest, sizeof dest) != 0) {
        push_toast(self, colecosession_last_error(self->session));
        return;
    }
    if (!colecosession_media_is_cartridge(dest)) {
        g_snprintf(msg, sizeof msg,
                   "Copied to FujiNet's SD folder \xe2\x80\x94 mount it from "
                   "the CONFIG client");
        push_toast(self, msg);
        return;
    }

    colecosession_set_str(self->session, "cart_path", dest);
    colecosession_settings_flush(self->session);
    {
        colecosession_start_opts o;
        colecosession_default_opts(self->session, &o);
        colecosession_stop(self->session);
        if (colecosession_start(self->session, &o) != 0) {
            push_toast(self, colecosession_last_error(self->session));
            return;
        }
    }
    g_snprintf(msg, sizeof msg, "Running %s", strrchr(dest, '/')
                                                  ? strrchr(dest, '/') + 1
                                                  : dest);
    push_toast(self, msg);
}

static void on_cart_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    ColecoWindow *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src),
                                                        res, NULL);
    g_autofree char *path = NULL;

    if (!file) return;
    path = g_file_get_path(file);
    if (path) load_media(self, path);
}

static void action_open(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    ColecoWindow *self = user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *carts = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    (void)a; (void)p;

    gtk_file_filter_set_name(carts, "ColecoVision cartridges");
    gtk_file_filter_add_pattern(carts, "*.rom");
    gtk_file_filter_add_pattern(carts, "*.col");
    gtk_file_filter_add_pattern(carts, "*.bin");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, carts);
    g_list_store_append(filters, all);

    gtk_file_dialog_set_title(dlg, "Open Cartridge");
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_cart_chosen, self);
    g_object_unref(carts);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dlg);
}

static void action_eject(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    ColecoWindow *self = user_data;
    (void)a; (void)p;
    colecosession_set_str(self->session, "cart_path", "");
    colecosession_settings_flush(self->session);
    colecosession_reset_to_config(self->session);
    push_toast(self, "Cartridge ejected");
}

/* Drag-and-drop, on the display where a cartridge visibly goes. */
static gboolean on_drop(GtkDropTarget *t, const GValue *value, double x,
                        double y, gpointer user_data)
{
    ColecoWindow *self = user_data;
    g_autofree char *path = NULL;
    (void)t; (void)x; (void)y;

    if (!G_VALUE_HOLDS(value, G_TYPE_FILE)) return FALSE;
    path = g_file_get_path(G_FILE(g_value_get_object(value)));
    if (!path) return FALSE;
    load_media(self, path);
    return TRUE;
}

static void action_keypad(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    ColecoWindow *self = user_data;
    (void)a; (void)p;
    coleco_keypad_window_toggle(GTK_WINDOW(self), self->session);
}

static void action_debugger(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    ColecoWindow *self = user_data;
    (void)a; (void)p;
    coleco_debugger_show(GTK_WINDOW(self), self->session);
}

static void action_reset_config(GSimpleAction *a, GVariant *p,
                                gpointer user_data)
{
    ColecoWindow *self = user_data;
    (void)a; (void)p;
    colecosession_reset_to_config(self->session);
    push_toast(self, "Back to the FujiNet CONFIG client");
}

static void action_reset(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    ColecoWindow *self = user_data;
    (void)a; (void)p;
    colecosession_reset(self->session);
    push_toast(self, "Console reset");
}

static void on_bios_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    ColecoWindow *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src),
                                                        res, NULL);
    g_autofree char *path = NULL;

    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;

    if (colecosession_import_bios(self->session, path) < 0) {
        push_toast(self, colecosession_last_error(self->session));
        return;
    }
    push_toast(self, "BIOS imported. Restart to boot it.");
}

static void action_import_bios(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    ColecoWindow *self = user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    (void)a; (void)p;
    gtk_file_dialog_set_title(dlg, "Import ColecoVision BIOS (OS7.rom)");
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_bios_chosen, self);
    g_object_unref(dlg);
}

static void action_fujinet_config(GSimpleAction *a, GVariant *p,
                                  gpointer user_data)
{
    ColecoWindow *self = user_data;
    GtkUriLauncher *l;
    (void)a; (void)p;

    if (!colecosession_fujinet_running(self->session)) {
        push_toast(self, "FujiNet is not running");
        return;
    }
    l = gtk_uri_launcher_new(colecosession_fujinet_webui_url(self->session));
    gtk_uri_launcher_launch(l, GTK_WINDOW(self), NULL, NULL, NULL);
    g_object_unref(l);
}

static void action_aspect(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    ColecoWindow *self = user_data;
    gboolean tv = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    g_simple_action_set_state(a, g_variant_new_boolean(tv));
    coleco_display_set_tv_aspect(COLECO_DISPLAY(self->display), tv);
    colecosession_set_int(self->session, "tv_aspect", tv ? 1 : 0);
}

static void action_smooth(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    ColecoWindow *self = user_data;
    gboolean sm = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    g_simple_action_set_state(a, g_variant_new_boolean(sm));
    coleco_display_set_smooth(COLECO_DISPLAY(self->display), sm);
    colecosession_set_int(self->session, "smooth", sm ? 1 : 0);
}

static const GActionEntry win_actions[] = {
    { "open", action_open, NULL, NULL, NULL, { 0 } },
    { "eject", action_eject, NULL, NULL, NULL, { 0 } },
    { "reset", action_reset, NULL, NULL, NULL, { 0 } },
    { "reset-config", action_reset_config, NULL, NULL, NULL, { 0 } },
    { "keypad", action_keypad, NULL, NULL, NULL, { 0 } },
    { "debugger", action_debugger, NULL, NULL, NULL, { 0 } },
    { "import-bios", action_import_bios, NULL, NULL, NULL, { 0 } },
    { "fujinet-config", action_fujinet_config, NULL, NULL, NULL, { 0 } },
    { "tv-aspect", action_aspect, NULL, "true", NULL, { 0 } },
    { "smooth", action_smooth, NULL, "false", NULL, { 0 } },
};

/* ---- construction --------------------------------------------------------- */

static GMenu *build_menu(void)
{
    GMenu *menu = g_menu_new();
    GMenu *machine = g_menu_new();
    GMenu *view = g_menu_new();
    GMenu *fuji = g_menu_new();

    g_menu_append(machine, "_Open Cartridge...", "win.open");
    g_menu_append(machine, "_Eject Cartridge", "win.eject");
    g_menu_append(machine, "_Reset Console", "win.reset");
    g_menu_append(machine, "Reset to _CONFIG", "win.reset-config");
    g_menu_append(machine, "_Import BIOS...", "win.import-bios");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(machine));

    g_menu_append(view, "_Controllers (F9)", "win.keypad");
    g_menu_append(view, "_Debugger (F12)", "win.debugger");
    g_menu_append(view, "_TV Aspect (4:3)", "win.tv-aspect");
    g_menu_append(view, "_Smooth Scaling", "win.smooth");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(view));

    g_menu_append(fuji, "FujiNet _Configuration", "win.fujinet-config");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(fuji));

    g_object_unref(machine);
    g_object_unref(view);
    g_object_unref(fuji);
    return menu;
}

static void coleco_window_dispose(GObject *object)
{
    ColecoWindow *self = COLECO_WINDOW(object);
    if (self->status_id) {
        g_source_remove(self->status_id);
        self->status_id = 0;
    }
    G_OBJECT_CLASS(coleco_window_parent_class)->dispose(object);
}

static void coleco_window_class_init(ColecoWindowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = coleco_window_dispose;
}

static void coleco_window_init(ColecoWindow *self)
{
    coleco_input_reset(&self->input);
}

GtkWidget *coleco_window_new(AdwApplication *app, colecosession *session)
{
    ColecoWindow *self = g_object_new(COLECO_TYPE_WINDOW,
                                      "application", app, NULL);
    GtkWidget *box, *header, *menu_button, *toolbar;
    GtkEventController *keys, *focus;
    g_autoptr(GMenu) menu = NULL;

    self->session = session;

    gtk_window_set_title(GTK_WINDOW(self), "FujiNet Go ColecoVision");
    /* 256x212 at 3x, in 4:3. Big enough that the BIOS text is readable
     * without the user having to resize on every launch. */
    gtk_window_set_default_size(GTK_WINDOW(self), 848, 636);

    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                    G_N_ELEMENTS(win_actions), self);

    header = adw_header_bar_new();
    menu = build_menu();
    menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button),
                                  "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button),
                                   G_MENU_MODEL(menu));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_button);

    self->status = gtk_label_new("Starting...");
    gtk_widget_add_css_class(self->status, "dim-label");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), self->status);

    self->display = coleco_display_new(session);
    coleco_display_set_tv_aspect(COLECO_DISPLAY(self->display),
        colecosession_get_int(session, "tv_aspect", 1) != 0);
    coleco_display_set_smooth(COLECO_DISPLAY(self->display),
        colecosession_get_int(session, "smooth", 0) != 0);

    {
        GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
        g_signal_connect(drop, "drop", G_CALLBACK(on_drop), self);
        gtk_widget_add_controller(self->display, GTK_EVENT_CONTROLLER(drop));
    }

    self->toast_overlay = adw_toast_overlay_new();
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(self->toast_overlay),
                                self->display);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar),
                                 self->toast_overlay);
    gtk_box_append(GTK_BOX(box), toolbar);
    gtk_widget_set_vexpand(toolbar, TRUE);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), box);

    /* Capture on the WINDOW, not the display widget, so input works no matter
     * what has focus -- there is no text entry here to compete with. */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), focus);

    self->status_id = g_timeout_add_seconds(1, update_status, self);
    update_status(self);

    /* COLECO_OPEN_KEYPAD=1 opens the controller panel at launch, following
     * the family's <T>_OPEN_DEBUGGER convention. Worth having for the same
     * reason: it is the way in when the app misbehaves before the menu is
     * reachable, and the way a headless check can look at the panel. */
    {
        const char *env = g_getenv("COLECO_OPEN_KEYPAD");
        if (env && *env && *env != '0')
            coleco_keypad_window_toggle(GTK_WINDOW(self), session);
        env = g_getenv("COLECO_OPEN_DEBUGGER");
        if (env && *env && *env != '0')
            coleco_debugger_show(GTK_WINDOW(self), session);
    }

    if (!colecosession_bios_available(session)) {
        push_toast(self, "No ColecoVision BIOS \xe2\x80\x94 use the menu to "
                         "import OS7.rom");
    }
    return GTK_WIDGET(self);
}
