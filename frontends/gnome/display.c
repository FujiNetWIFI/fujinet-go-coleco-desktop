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

/* ---- frame buffer pool ----------------------------------------------------
 * The converted frame is written straight into a pooled buffer and the
 * texture borrows it: no allocation in steady state. The buffer comes back to
 * the pool when GSK drops the texture's bytes, possibly after the widget is
 * gone, so the pool is reference counted and each outstanding buffer holds a
 * reference. */
typedef struct {
    gatomicrefcount refs;
    GMutex lock;
    GPtrArray *spare;      /* idle guint32 buffers, FB_W * FB_H each */
} FramePool;

typedef struct {
    FramePool *pool;
    guint32 *px;
} FrameLoan;

#define POOL_KEEP 4

static FramePool *pool_new(void)
{
    FramePool *p = g_new0(FramePool, 1);
    g_atomic_ref_count_init(&p->refs);
    g_mutex_init(&p->lock);
    p->spare = g_ptr_array_new_with_free_func(g_free);
    return p;
}

static void pool_unref(FramePool *p)
{
    if (!g_atomic_ref_count_dec(&p->refs))
        return;
    g_ptr_array_free(p->spare, TRUE);
    g_mutex_clear(&p->lock);
    g_free(p);
}

static guint32 *pool_take(FramePool *p)
{
    guint32 *px = NULL;

    g_mutex_lock(&p->lock);
    if (p->spare->len > 0)
        px = g_ptr_array_steal_index(p->spare, p->spare->len - 1);
    g_mutex_unlock(&p->lock);
    return px ? px : g_malloc((gsize)FB_W * FB_H * sizeof(guint32));
}

static void pool_give(FramePool *p, guint32 *px)
{
    g_mutex_lock(&p->lock);
    if (p->spare->len < POOL_KEEP) {
        g_ptr_array_add(p->spare, px);
        px = NULL;
    }
    g_mutex_unlock(&p->lock);
    g_free(px);
}

static void loan_release(gpointer data)
{
    FrameLoan *loan = data;

    pool_give(loan->pool, loan->px);
    pool_unref(loan->pool);
    g_free(loan);
}

struct _ColecoDisplay {
    GtkWidget parent_instance;

    colecosession *session;
    uint16_t *fb;          /* RGB565 straight from the session */
    FramePool *pool;
    GdkTexture *texture;
    guint64 serial;
    guint tick_id;
    gulong suspended_id;   /* notify::suspended on the toplevel, while mapped */
    GtkWindow *toplevel;
    gboolean tv_aspect;
    gboolean smooth;
};

G_DEFINE_FINAL_TYPE(ColecoDisplay, coleco_display, GTK_TYPE_WIDGET)

/* RGB565 -> XRGB8888, the layout GDK_MEMORY_B8G8R8X8 wants on a little-endian
 * host (the X byte is ignored, so no alpha). The 5- and 6-bit fields
 * are expanded by replicating their high bits, not by shifting left: a plain
 * shift makes full-scale 0x1F map to 0xF8 rather than 0xFF, so white comes
 * out slightly grey and every colour is a touch dark. */
static inline guint32 rgb565_to_xrgb(uint16_t p)
{
    guint r = (p >> 11) & 0x1F;
    guint g = (p >> 5) & 0x3F;
    guint b = p & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

/* Convert the session's RGB565 frame into px (one pass) and wrap px in the
 * texture. The previous texture goes in as the update source so GSK can reuse
 * its GPU texture. Takes ownership of px. */
static void publish_texture(ColecoDisplay *self, guint32 *px)
{
    GdkMemoryTextureBuilder *b = gdk_memory_texture_builder_new();
    FrameLoan *loan = g_new(FrameLoan, 1);
    GBytes *bytes;
    int i;

    for (i = 0; i < FB_W * FB_H; i++)
        px[i] = rgb565_to_xrgb(self->fb[i]);

    g_atomic_ref_count_inc(&self->pool->refs);
    loan->pool = self->pool;
    loan->px = px;
    bytes = g_bytes_new_with_free_func(px, (gsize)FB_W * FB_H * sizeof(guint32),
                                       loan_release, loan);
    gdk_memory_texture_builder_set_bytes(b, bytes);
    gdk_memory_texture_builder_set_stride(b, (gsize)FB_W * 4);
    gdk_memory_texture_builder_set_width(b, FB_W);
    gdk_memory_texture_builder_set_height(b, FB_H);
    gdk_memory_texture_builder_set_format(b, GDK_MEMORY_B8G8R8X8);
    if (self->texture) {
        cairo_rectangle_int_t all = { 0, 0, FB_W, FB_H };
        cairo_region_t *region = cairo_region_create_rectangle(&all);

        gdk_memory_texture_builder_set_update_texture(b, self->texture);
        gdk_memory_texture_builder_set_update_region(b, region);
        cairo_region_destroy(region);
    }
    g_clear_object(&self->texture);
    self->texture = gdk_memory_texture_builder_build(b);
    g_bytes_unref(bytes);
    g_object_unref(b);
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
        publish_texture(self, pool_take(self->pool));
        gtk_widget_queue_draw(widget);
    }
    return G_SOURCE_CONTINUE;
}

/* The tick runs only while somebody can see the result. A hidden or
 * suspended window gets no frame callbacks from the compositor anyway, but a
 * minimised one on some stacks keeps ticking, and every tick is a wakeup.
 * The emulator does not care: with no ticks the vsync stream goes stale and
 * the session paces on the wall clock. The first tick after the window comes
 * back copies whatever frame is newest. */
static void update_tick(ColecoDisplay *self)
{
    gboolean want = gtk_widget_get_mapped(GTK_WIDGET(self)) &&
                    !(self->toplevel && gtk_window_is_suspended(self->toplevel));

    if (want && !self->tick_id)
        self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), on_tick,
                                                     NULL, NULL);
    else if (!want && self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
}

static void on_suspended(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void)obj; (void)pspec;
    update_tick(COLECO_DISPLAY(user_data));
}

static void coleco_display_map(GtkWidget *widget)
{
    ColecoDisplay *self = COLECO_DISPLAY(widget);
    GtkRoot *root;

    GTK_WIDGET_CLASS(coleco_display_parent_class)->map(widget);
    root = gtk_widget_get_root(widget);
    if (GTK_IS_WINDOW(root)) {
        self->toplevel = GTK_WINDOW(root);
        self->suspended_id = g_signal_connect(self->toplevel, "notify::suspended",
                                              G_CALLBACK(on_suspended), self);
    }
    update_tick(self);
}

static void coleco_display_unmap(GtkWidget *widget)
{
    ColecoDisplay *self = COLECO_DISPLAY(widget);

    if (self->toplevel && self->suspended_id)
        g_signal_handler_disconnect(self->toplevel, self->suspended_id);
    self->suspended_id = 0;
    self->toplevel = NULL;
    GTK_WIDGET_CLASS(coleco_display_parent_class)->unmap(widget);
    update_tick(self);
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
    if (self->pool) {
        pool_unref(self->pool);
        self->pool = NULL;
    }

    G_OBJECT_CLASS(coleco_display_parent_class)->dispose(object);
}

static void coleco_display_class_init(ColecoDisplayClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = coleco_display_dispose;
    widget_class->snapshot = coleco_display_snapshot;
    widget_class->map = coleco_display_map;
    widget_class->unmap = coleco_display_unmap;
}

static void coleco_display_init(ColecoDisplay *self)
{
    self->fb = g_new0(uint16_t, FB_W * FB_H);
    self->pool = pool_new();
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
