/*
 * display.c -- see display.h.
 *
 * Two things here are load bearing and neither is obvious:
 *
 *  - The frame clock is the emulator's clock. GTK4 presents on the
 *    compositor's vsync, so its tick callback is the most accurate ~60 Hz
 *    signal available, and feeding it to the session lets the emulator run
 *    one frame per tick instead of racing its own timer against the panel's.
 *    Without it a 59.92 Hz machine on a 60 Hz display beats slowly, which
 *    reads as a stutter roughly once every twelve seconds.
 *
 *  - Frames are pulled by serial, not pushed. copy_frame does nothing when
 *    the emulator has not produced a new frame, so a tick that arrives
 *    between frames costs a mutex and no memcpy.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"

#include <string.h>

#define FB_W COLECOSESSION_FB_WIDTH
#define FB_H COLECOSESSION_FB_HEIGHT

struct _ColecoDisplay {
    GtkWidget parent_instance;

    colecosession *session;
    uint16_t *fb;          /* RGB565 straight from the session */
    guint32 *rgba;         /* converted for GdkMemoryTexture */
    GdkTexture *texture;
    guint64 serial;
    guint tick_id;
    gboolean tv_aspect;
    gboolean smooth;
};

G_DEFINE_FINAL_TYPE(ColecoDisplay, coleco_display, GTK_TYPE_WIDGET)

/* RGB565 -> premultiplied-irrelevant opaque BGRA, the layout
 * GDK_MEMORY_B8G8R8A8 wants on a little-endian host. The 5- and 6-bit fields
 * are expanded by replicating their high bits, not by shifting left: a plain
 * shift makes full-scale 0x1F map to 0xF8 rather than 0xFF, so white comes
 * out slightly grey and every colour is a touch dark. */
static inline guint32 rgb565_to_bgra(uint16_t p)
{
    guint r = (p >> 11) & 0x1F;
    guint g = (p >> 5) & 0x3F;
    guint b = p & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void rebuild_texture(ColecoDisplay *self)
{
    GBytes *bytes;
    int i;

    for (i = 0; i < FB_W * FB_H; i++)
        self->rgba[i] = rgb565_to_bgra(self->fb[i]);

    bytes = g_bytes_new_static(self->rgba, (gsize)FB_W * FB_H * 4);
    g_clear_object(&self->texture);
    self->texture = gdk_memory_texture_new(FB_W, FB_H, GDK_MEMORY_B8G8R8A8,
                                           bytes, (gsize)FB_W * 4);
    g_bytes_unref(bytes);
}

static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data)
{
    ColecoDisplay *self = COLECO_DISPLAY(widget);
    (void)user_data;

    /* Hand the emulator the compositor's cadence. */
    colecosession_notify_vsync(self->session,
                              gdk_frame_clock_get_frame_time(clock) * 1000);

    if (colecosession_copy_frame(self->session, self->fb, &self->serial)) {
        rebuild_texture(self);
        gtk_widget_queue_draw(widget);
    }
    return G_SOURCE_CONTINUE;
}

static void coleco_display_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    ColecoDisplay *self = COLECO_DISPLAY(widget);
    int w = gtk_widget_get_width(widget);
    int h = gtk_widget_get_height(widget);
    double want, sw, sh, x, y;
    graphene_rect_t rect;

    /* The ground behind the picture. A ColecoVision fills its overscan with
     * the backdrop colour, so black bars are what a television would have
     * shown outside the raster. */
    gtk_snapshot_append_color(snapshot, &(GdkRGBA){ 0, 0, 0, 1 },
                              &GRAPHENE_RECT_INIT(0, 0, w, h));
    if (!self->texture || w <= 0 || h <= 0)
        return;

    /* 256x212 is not 4:3: the 192 active lines are, and the 10-line borders
     * top and bottom are part of the same 4:3 field on real hardware. So the
     * TV mode stretches the whole 256x212 buffer to 4:3 rather than
     * letterboxing it inside one. */
    want = self->tv_aspect ? (4.0 / 3.0) : ((double)FB_W / (double)FB_H);

    if ((double)w / (double)h > want) {
        sh = h;
        sw = sh * want;
    } else {
        sw = w;
        sh = sw / want;
    }
    x = (w - sw) / 2.0;
    y = (h - sh) / 2.0;

    graphene_rect_init(&rect, (float)x, (float)y, (float)sw, (float)sh);
    /* Nearest by default: this is a 256-pixel-wide image being blown up
     * several times, and bilinear smearing is not what the machine looked
     * like. Smooth is offered for people who prefer it. */
    gtk_snapshot_append_scaled_texture(
        snapshot, self->texture,
        self->smooth ? GSK_SCALING_FILTER_LINEAR : GSK_SCALING_FILTER_NEAREST,
        &rect);
}

static void coleco_display_dispose(GObject *object)
{
    ColecoDisplay *self = COLECO_DISPLAY(object);

    if (self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
    g_clear_object(&self->texture);
    g_clear_pointer(&self->fb, g_free);
    g_clear_pointer(&self->rgba, g_free);

    G_OBJECT_CLASS(coleco_display_parent_class)->dispose(object);
}

static void coleco_display_class_init(ColecoDisplayClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = coleco_display_dispose;
    widget_class->snapshot = coleco_display_snapshot;
}

static void coleco_display_init(ColecoDisplay *self)
{
    self->fb = g_new0(uint16_t, FB_W * FB_H);
    self->rgba = g_new0(guint32, FB_W * FB_H);
    self->tv_aspect = TRUE;
    self->smooth = FALSE;
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
}

GtkWidget *coleco_display_new(colecosession *session)
{
    ColecoDisplay *self = g_object_new(COLECO_TYPE_DISPLAY, NULL);
    self->session = session;
    self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), on_tick,
                                                 NULL, NULL);
    return GTK_WIDGET(self);
}

void coleco_display_set_tv_aspect(ColecoDisplay *self, gboolean tv)
{
    self->tv_aspect = tv;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void coleco_display_set_smooth(ColecoDisplay *self, gboolean smooth)
{
    self->smooth = smooth;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
