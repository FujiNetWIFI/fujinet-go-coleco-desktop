/*
 * ColecoDebuggerWindow -- see debugger_window.h.
 *
 * Two tabs over one engine: the Z80 (registers, disassembly with breakpoint
 * markers, a hex memory view) and the VDP (all eight registers decoded, plus
 * the status and address latch). The engine does the work; this file is a
 * window.
 *
 * Refresh is on a timer, not on every frame, and only while the window is
 * VISIBLE. Reading the machine is not free -- disassembly and a memory view
 * peek hundreds of bytes -- and doing it behind a hidden window would slow
 * the emulator for no one's benefit.
 *
 * Keys follow the family: F12 opens, F5 pause/continue, F7 step into,
 * F8 step over, Shift+F8 step out.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "debugger_window.h"

#include <stdio.h>
#include <string.h>

#include "colecodebug.h"

#define DISASM_LINES 24
#define MEM_ROWS     16

static GtkWidget *g_window;
static colecosession *g_session;
static colecodebug *g_dbg;
static GtkWidget *g_regs, *g_disasm, *g_mem, *g_vdp, *g_state;
static GtkWidget *g_pause_btn;
static guint g_timer;
static uint16_t g_mem_addr = 0x0000;
static uint16_t g_disasm_addr;
static int g_follow_pc = 1;

/* ---- rendering ------------------------------------------------------------ */

static void render_regs(void)
{
    adamcore_z80_regs r;
    char buf[640];

    colecodebug_get_regs(g_dbg, &r);
    g_snprintf(buf, sizeof buf,
        "PC %04X   SP %04X   IX %04X   IY %04X\n"
        "AF %02X%02X   BC %02X%02X   DE %02X%02X   HL %02X%02X\n"
        "AF'%02X%02X   BC'%02X%02X   DE'%02X%02X   HL'%02X%02X\n"
        "I %02X  R %02X  IM %u  IFF %u/%u  WZ %04X  %s\n"
        "cycles %llu",
        r.pc, r.sp, r.ix, r.iy,
        r.a, r.f, r.b, r.c, r.d, r.e, r.h, r.l,
        r.a2, r.f2, r.b2, r.c2, r.d2, r.e2, r.h2, r.l2,
        r.i, r.r, r.im, r.iff1, r.iff2, r.wz,
        r.halted ? "HALTED" : "",
        (unsigned long long)r.cycles);
    gtk_label_set_text(GTK_LABEL(g_regs), buf);
}

static void render_disasm(void)
{
    colecodasm_line lines[DISASM_LINES];
    adamcore_z80_regs r;
    GString *out = g_string_new(NULL);
    int n, i;

    colecodebug_get_regs(g_dbg, &r);
    if (g_follow_pc) g_disasm_addr = r.pc;

    n = colecodebug_disassemble(g_dbg, g_disasm_addr, DISASM_LINES, lines);
    for (i = 0; i < n; i++) {
        char bytes[16] = "";
        int b;
        for (b = 0; b < lines[i].len && b < 4; b++)
            g_snprintf(bytes + b * 3, sizeof bytes - (gsize)b * 3, "%02X ",
                       lines[i].bytes[b]);
        g_string_append_printf(out, "%c%c %04X  %-12s %s%s%s\n",
            colecodebug_bp_is_set(g_dbg, lines[i].addr) ? '*' : ' ',
            lines[i].addr == r.pc ? '>' : ' ',
            lines[i].addr, bytes, lines[i].text,
            lines[i].symbol ? "   ; " : "",
            lines[i].symbol ? lines[i].symbol : "");
    }
    gtk_label_set_text(GTK_LABEL(g_disasm), out->str);
    g_string_free(out, TRUE);
}

static void render_mem(void)
{
    GString *out = g_string_new(NULL);
    int row;

    for (row = 0; row < MEM_ROWS; row++) {
        uint8_t b[16];
        uint16_t a = (uint16_t)(g_mem_addr + row * 16);
        int i;
        colecodebug_read_mem(g_dbg, a, b, 16);
        g_string_append_printf(out, "%04X  ", a);
        for (i = 0; i < 16; i++) g_string_append_printf(out, "%02X ", b[i]);
        g_string_append(out, " ");
        for (i = 0; i < 16; i++)
            g_string_append_c(out, (b[i] >= 0x20 && b[i] < 0x7F) ? (char)b[i] : '.');
        g_string_append_c(out, '\n');
    }
    gtk_label_set_text(GTK_LABEL(g_mem), out->str);
    g_string_free(out, TRUE);
}

static void render_vdp(void)
{
    colecovdp_snapshot snap;
    GString *out = g_string_new(NULL);
    int i;

    colecodebug_vdp_snapshot(g_dbg, &snap);
    for (i = 0; i < 8; i++) {
        char desc[128];
        colecovdp_describe_register(&snap, i, desc, sizeof desc);
        g_string_append_printf(out, "R%d = %02X   %s\n", i, snap.regs[i], desc);
    }
    {
        char desc[128];
        colecovdp_describe_status(&snap, desc, sizeof desc);
        g_string_append_printf(out, "\nstatus = %02X   %s\n", snap.status, desc);
        g_string_append_printf(out, "address latch = %04X\n", snap.addr);
    }
    gtk_label_set_text(GTK_LABEL(g_vdp), out->str);
    g_string_free(out, TRUE);
}

static gboolean refresh(gpointer data)
{
    (void)data;
    /* Only while visible: reading the machine costs real work, and doing it
     * behind a hidden window would slow the emulator for nobody. */
    if (!g_window || !gtk_widget_get_visible(g_window))
        return G_SOURCE_CONTINUE;

    gtk_label_set_text(GTK_LABEL(g_state),
                       colecodebug_is_paused(g_dbg) ? "PAUSED" : "running");
    gtk_button_set_label(GTK_BUTTON(g_pause_btn),
                         colecodebug_is_paused(g_dbg) ? "Continue" : "Pause");
    render_regs();
    render_disasm();
    render_mem();
    render_vdp();
    return G_SOURCE_CONTINUE;
}

/* ---- controls ------------------------------------------------------------- */

static void on_pause(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (colecodebug_is_paused(g_dbg)) colecodebug_resume(g_dbg);
    else colecodebug_pause(g_dbg);
    refresh(NULL);
}

static void on_step_into(GtkButton *b, gpointer d)
{ (void)b; (void)d; colecodebug_step_into(g_dbg); }
static void on_step_over(GtkButton *b, gpointer d)
{ (void)b; (void)d; colecodebug_step_over(g_dbg); }
static void on_step_out(GtkButton *b, gpointer d)
{ (void)b; (void)d; colecodebug_step_out(g_dbg); }

static void on_follow(GtkCheckButton *c, gpointer d)
{
    (void)d;
    g_follow_pc = gtk_check_button_get_active(c);
}

static void on_mem_addr(GtkEditable *e, gpointer d)
{
    unsigned v;
    (void)d;
    if (sscanf(gtk_editable_get_text(e), "%x", &v) == 1)
        g_mem_addr = (uint16_t)v;
}

static void on_bp_addr(GtkEntry *e, gpointer d)
{
    unsigned v;
    (void)d;
    if (sscanf(gtk_editable_get_text(GTK_EDITABLE(e)), "%x", &v) == 1) {
        colecodebug_bp_toggle(g_dbg, (uint16_t)v);
        refresh(NULL);
    }
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType state, gpointer d)
{
    (void)c; (void)code; (void)d;
    switch (keyval) {
    case GDK_KEY_F5: on_pause(NULL, NULL); return TRUE;
    case GDK_KEY_F7: colecodebug_step_into(g_dbg); return TRUE;
    case GDK_KEY_F8:
        if (state & GDK_SHIFT_MASK) colecodebug_step_out(g_dbg);
        else colecodebug_step_over(g_dbg);
        return TRUE;
    default: return FALSE;
    }
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
    (void)d;
    /* Let the machine go when the window closes: leaving it paused behind a
     * closed debugger looks exactly like a hung emulator. */
    if (colecodebug_is_paused(g_dbg)) colecodebug_resume(g_dbg);
    gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
    return TRUE;
}

/* ---- construction --------------------------------------------------------- */

static GtkWidget *mono_label(void)
{
    GtkWidget *l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_add_css_class(l, "monospace");
    return l;
}

static GtkWidget *scrolled(GtkWidget *child, int h)
{
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), child);
    gtk_widget_set_size_request(sw, -1, h);
    gtk_widget_set_vexpand(sw, TRUE);
    return sw;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *tabs, *z80, *vdp, *controls, *header, *toolbar, *row;
    GtkWidget *follow, *memaddr, *bpaddr;
    GtkEventController *keys;

    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Debugger");
    gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
    gtk_window_set_default_size(GTK_WINDOW(g_window), 720, 780);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);

    controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    g_pause_btn = gtk_button_new_with_label("Pause");
    g_signal_connect(g_pause_btn, "clicked", G_CALLBACK(on_pause), NULL);
    gtk_box_append(GTK_BOX(controls), g_pause_btn);
    row = gtk_button_new_with_label("Step In (F7)");
    g_signal_connect(row, "clicked", G_CALLBACK(on_step_into), NULL);
    gtk_box_append(GTK_BOX(controls), row);
    row = gtk_button_new_with_label("Step Over (F8)");
    g_signal_connect(row, "clicked", G_CALLBACK(on_step_over), NULL);
    gtk_box_append(GTK_BOX(controls), row);
    row = gtk_button_new_with_label("Step Out");
    g_signal_connect(row, "clicked", G_CALLBACK(on_step_out), NULL);
    gtk_box_append(GTK_BOX(controls), row);
    g_state = gtk_label_new("running");
    gtk_widget_add_css_class(g_state, "dim-label");
    gtk_box_append(GTK_BOX(controls), g_state);

    z80 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(z80, 10);
    gtk_widget_set_margin_bottom(z80, 10);
    gtk_widget_set_margin_start(z80, 10);
    gtk_widget_set_margin_end(z80, 10);
    gtk_box_append(GTK_BOX(z80), controls);

    g_regs = mono_label();
    gtk_box_append(GTK_BOX(z80), g_regs);

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    follow = gtk_check_button_new_with_label("Follow PC");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(follow), TRUE);
    g_signal_connect(follow, "toggled", G_CALLBACK(on_follow), NULL);
    gtk_box_append(GTK_BOX(row), follow);
    bpaddr = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(bpaddr), "breakpoint hex addr");
    g_signal_connect(bpaddr, "activate", G_CALLBACK(on_bp_addr), NULL);
    gtk_box_append(GTK_BOX(row), bpaddr);
    gtk_box_append(GTK_BOX(z80), row);

    g_disasm = mono_label();
    gtk_box_append(GTK_BOX(z80), scrolled(g_disasm, 320));

    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(row), gtk_label_new("Memory at"));
    memaddr = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(memaddr), "0000");
    g_signal_connect(memaddr, "changed", G_CALLBACK(on_mem_addr), NULL);
    gtk_box_append(GTK_BOX(row), memaddr);
    gtk_box_append(GTK_BOX(z80), row);

    g_mem = mono_label();
    gtk_box_append(GTK_BOX(z80), scrolled(g_mem, 220));

    vdp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(vdp, 10);
    gtk_widget_set_margin_start(vdp, 10);
    gtk_widget_set_margin_end(vdp, 10);
    g_vdp = mono_label();
    gtk_box_append(GTK_BOX(vdp), scrolled(g_vdp, 400));

    tabs = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), z80, gtk_label_new("Z80"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), vdp, gtk_label_new("VDP"));

    header = adw_header_bar_new();
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), tabs);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), NULL);
    gtk_widget_add_controller(g_window, keys);

    g_timer = g_timeout_add(120, refresh, NULL);
}

void coleco_debugger_window_toggle(GtkWindow *parent, colecosession *session)
{
    g_session = session;
    g_dbg = colecosession_debugger(session);
    if (!g_dbg) return;
    if (!g_window) build_window(parent);

    if (gtk_widget_get_visible(g_window)) {
        if (colecodebug_is_paused(g_dbg)) colecodebug_resume(g_dbg);
        gtk_widget_set_visible(g_window, FALSE);
    } else {
        gtk_window_present(GTK_WINDOW(g_window));
        refresh(NULL);
    }
}
