/*
 * ColecoKeypadWindow -- both hand controllers side by side: a 3x4 keypad
 * (1-9, then * 0 #), a four-way direction pad and the two fire buttons,
 * each driving colecosession_press directly.
 *
 * Every control is pressed with a raw GtkGestureClick (press AND release),
 * not GtkButton's "clicked". This is the difference between working and not:
 * the emulator samples the controller register once per frame, so a value
 * present only for the instant of a click falls between frames and a polling
 * game -- which is how essentially every ColecoVision title reads its skill
 * select -- never sees it. A button on screen is HELD for as long as the
 * mouse button is down, exactly like the plastic one.
 *
 * The gesture's "cancelled" is wired to the same release path as "released".
 * Dragging off a button, or the window losing the grab, must not leave the
 * machine believing a key is still down forever.
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing the
 * window.
 *
 * FOCUS: this window forwards keyboard events to the session exactly like
 * the main window does, rather than trying to refuse focus outright -- GTK4
 * gives an application no reliable cross-platform way to stop a toplevel
 * being focused on click, so instead of fighting that, typing still drives
 * the machine whichever window the window manager currently has focused.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click any control above
 * to pick the target, then press a keyboard key to bind it. While armed, the
 * same press/release gestures pick the target instead of injecting input,
 * and the key handler intercepts the next keystroke instead of forwarding
 * it. Rebinding steals the key from whatever held it (core/src/bindings.c).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keypad_window.h"

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static colecosession *g_session;
static coleco_input_state g_unused;   /* the session owns the panel's state */

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button;
static GtkWidget *g_map_hint;

/* Every control button, so Map mode can relabel them all at once. */
typedef struct {
    GtkWidget *button;
    int target;      /* COLECO_TARGET_* index */
} control;

static control g_controls[COLECO_TARGET_COUNT];
static int g_ncontrols;

static void refresh_labels(void);

/* ---- pressing ------------------------------------------------------------- */

static void press_target(int target, int down)
{
    if (target >= COLECO_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off should not reset the console. */
        if (!down)
            colecosession_sysaction(g_session, target - COLECO_TARGET_SYSACT(0));
        return;
    }
    colecosession_press(g_session, target / COLECO_ACT_PER_PORT,
                        target % COLECO_ACT_PER_PORT, down);
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state == -2) {
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "suggested-action");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "");
    } else if (state == -1) {
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Cancel");
        gtk_widget_add_css_class(g_map_button, "suggested-action");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "Click a control to remap");
    } else {
        char msg[96];
        g_snprintf(msg, sizeof msg, "Press a key for %s",
                   coleco_binding_label(state));
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
    }
    refresh_labels();
}

static void on_pressed(GtkGestureClick *g, int n, double x, double y,
                       gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)n; (void)x; (void)y;

    if (g_map_state == -1) {
        set_map_state(target);
        return;
    }
    if (g_map_state >= 0)
        return;   /* waiting for a key; ignore further clicks */
    press_target(target, 1);
}

static void on_released(GtkGestureClick *g, int n, double x, double y,
                        gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)n; (void)x; (void)y;
    if (g_map_state != -2) return;
    press_target(target, 0);
}

/* Dragging off a button, or losing the grab, must release it -- otherwise
 * the machine believes it is held forever. */
static void on_cancelled(GtkGesture *g, GdkEventSequence *seq,
                         gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)seq;
    if (g_map_state != -2) return;
    press_target(target, 0);
}

static GtkWidget *control_button(const char *label, int target, int wide)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    GtkGesture *g = gtk_gesture_click_new();

    gtk_widget_set_size_request(b, wide ? 96 : 52, 42);
    /* Raw press/release, never "clicked": a keypad key is held. */
    g_signal_connect(g, "pressed", G_CALLBACK(on_pressed),
                     GINT_TO_POINTER(target));
    g_signal_connect(g, "released", G_CALLBACK(on_released),
                     GINT_TO_POINTER(target));
    g_signal_connect(g, "cancel", G_CALLBACK(on_cancelled),
                     GINT_TO_POINTER(target));
    gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(g));
    /* Not focusable: clicking a pad button must not steal focus from the
     * display, and tabbing through 38 buttons is nobody's idea of input. */
    gtk_widget_set_focusable(b, FALSE);

    if (g_ncontrols < COLECO_TARGET_COUNT) {
        g_controls[g_ncontrols].button = b;
        g_controls[g_ncontrols].target = target;
        g_ncontrols++;
    }
    return b;
}

/* In Map mode each control shows the key currently bound to it, so choosing
 * what to change does not require remembering the whole table. */
static void refresh_labels(void)
{
    int i;
    for (i = 0; i < g_ncontrols; i++) {
        int t = g_controls[i].target;
        GtkWidget *b = g_controls[i].button;
        if (g_map_state != -2) {
            const char *k = coleco_binding_key_name(t);
            gtk_button_set_label(GTK_BUTTON(b), *k ? k : "\xe2\x80\x94");
            if (t == g_map_state) gtk_widget_add_css_class(b, "suggested-action");
            else gtk_widget_remove_css_class(b, "suggested-action");
        } else {
            gtk_widget_remove_css_class(b, "suggested-action");
            gtk_button_set_label(GTK_BUTTON(b),
                                 g_object_get_data(G_OBJECT(b), "face"));
        }
    }
}

static GtkWidget *faced_button(const char *face, int target, int wide)
{
    GtkWidget *b = control_button(face, target, wide);
    g_object_set_data_full(G_OBJECT(b), "face", g_strdup(face), g_free);
    return b;
}

/* ---- one controller ------------------------------------------------------- */

static const char *const keypad_face[COLECO_KEYPAD_KEYS] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "#"
};
/* The physical 3x4 layout: 1-9 in reading order, then * 0 #. */
static const int keypad_order[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11 };

static GtkWidget *build_controller(int port)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *pad = gtk_grid_new();
    GtkWidget *dpad = gtk_grid_new();
    GtkWidget *fires = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *title;
    char label[24];
    int i;

    g_snprintf(label, sizeof label, "Controller %d", port + 1);
    title = gtk_label_new(label);
    gtk_widget_add_css_class(title, "heading");

    /* the 12-key pad */
    gtk_grid_set_row_spacing(GTK_GRID(pad), 6);
    gtk_grid_set_column_spacing(GTK_GRID(pad), 6);
    for (i = 0; i < 12; i++) {
        int key = keypad_order[i];
        gtk_grid_attach(GTK_GRID(pad),
                        faced_button(keypad_face[key],
                                     COLECO_TARGET_PORT(port,
                                         COLECO_ACT_KEYPAD + key), 0),
                        i % 3, i / 3, 1, 1);
    }

    /* the four-way stick */
    gtk_grid_set_row_spacing(GTK_GRID(dpad), 4);
    gtk_grid_set_column_spacing(GTK_GRID(dpad), 4);
    gtk_grid_attach(GTK_GRID(dpad),
        faced_button("\xe2\x96\xb2", COLECO_TARGET_PORT(port, COLECO_ACT_UP), 0),
        1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(dpad),
        faced_button("\xe2\x97\x80", COLECO_TARGET_PORT(port, COLECO_ACT_LEFT), 0),
        0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(dpad),
        faced_button("\xe2\x96\xb6", COLECO_TARGET_PORT(port, COLECO_ACT_RIGHT), 0),
        2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(dpad),
        faced_button("\xe2\x96\xbc", COLECO_TARGET_PORT(port, COLECO_ACT_DOWN), 0),
        1, 2, 1, 1);

    gtk_box_append(GTK_BOX(fires),
        faced_button("Fire L", COLECO_TARGET_PORT(port, COLECO_ACT_FIRE_L), 1));
    gtk_box_append(GTK_BOX(fires),
        faced_button("Fire R", COLECO_TARGET_PORT(port, COLECO_ACT_FIRE_R), 1));

    gtk_widget_set_halign(pad, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(dpad, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(fires, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), pad);
    gtk_box_append(GTK_BOX(box), dpad);
    gtk_box_append(GTK_BOX(box), fires);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_frame_set_child(GTK_FRAME(frame), box);
    return frame;
}

/* ---- keyboard ------------------------------------------------------------- */

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval,
                               guint code, GdkModifierType st, gpointer d)
{
    (void)c; (void)code; (void)st; (void)d;

    if (g_map_state >= 0) {
        coleco_binding_set(g_session, g_map_state, keyval);
        set_map_state(-1);   /* stay armed: remapping several in a row is
                              * the normal case */
        return TRUE;
    }
    if (g_map_state == -1)
        return TRUE;   /* swallow keys while choosing a target */

    /* Otherwise behave exactly like the main window, so typing works
     * whichever window has focus. */
    {
        int sa = coleco_input_key_sysaction(keyval);
        if (sa >= 0) {
            colecosession_sysaction(g_session, sa);
            return TRUE;
        }
    }
    if (coleco_input_key(&g_unused, keyval, 1)) {
        colecosession_joystick_raw(g_session, 0, coleco_input_word(&g_unused, 0));
        colecosession_joystick_raw(g_session, 1, coleco_input_word(&g_unused, 1));
        return TRUE;
    }
    return FALSE;
}

static gboolean on_key_released(GtkEventControllerKey *c, guint keyval,
                                guint code, GdkModifierType st, gpointer d)
{
    (void)c; (void)code; (void)st; (void)d;
    if (g_map_state != -2) return TRUE;
    if (coleco_input_key(&g_unused, keyval, 0)) {
        colecosession_joystick_raw(g_session, 0, coleco_input_word(&g_unused, 0));
        colecosession_joystick_raw(g_session, 1, coleco_input_word(&g_unused, 1));
        return TRUE;
    }
    return FALSE;
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_defaults_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    coleco_bindings_reset_defaults(g_session);
    refresh_labels();
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
    (void)d;
    /* Hide, do not destroy: a remap in progress and the window's position
     * both survive closing it. */
    set_map_state(-2);
    gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
    return TRUE;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *ports, *system, *maprow, *toolbar, *header, *defaults;
    GtkEventController *keys;

    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);

    coleco_input_reset(&g_unused);
    g_ncontrols = 0;

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    ports = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(ports), build_controller(0));
    gtk_box_append(GTK_BOX(ports), build_controller(1));
    gtk_box_append(GTK_BOX(root), ports);

    system = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(system, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(system),
        faced_button("Reset Console",
                     COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET), 1));
    gtk_box_append(GTK_BOX(system),
        faced_button("Reset to CONFIG",
                     COLECO_TARGET_SYSACT(COLECO_SYSACT_RESET_CONFIG), 1));
    gtk_box_append(GTK_BOX(root), system);

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_button_new_with_label("Map");
    g_signal_connect(g_map_button, "clicked", G_CALLBACK(on_map_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    gtk_box_append(GTK_BOX(maprow), g_map_hint);
    gtk_box_append(GTK_BOX(root), maprow);

    header = adw_header_bar_new();
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    set_map_state(-2);
}

void coleco_keypad_window_toggle(GtkWindow *parent, colecosession *session)
{
    g_session = session;
    if (!g_window)
        build_window(parent);

    if (gtk_widget_get_visible(g_window)) {
        set_map_state(-2);
        gtk_widget_set_visible(g_window, FALSE);
    } else {
        gtk_window_present(GTK_WINDOW(g_window));
    }
}

gboolean coleco_keypad_window_is_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}
