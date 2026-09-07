/*
 * fujinet_cart -- the FujiNet cartridge. See fujinet_cart.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fujinet_cart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The cartridge firmware's own sources, staged verbatim. */
#include "fuji_mailbox.h"
#include "fujimail.h"
#include "fujitcp.h"
#include "fujiconfigrom.h"

/* ---- a growable byte buffer for the DBC push streams ---------------------- */

typedef struct {
    uint8_t *p;
    uint32_t len, cap;
} bytebuf;

static void bb_clear(bytebuf *b) { b->len = 0; }

static void bb_free(bytebuf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

static int bb_reserve(bytebuf *b, uint32_t want)
{
    uint8_t *n;
    if (want <= b->cap) return 0;
    if (want < 1024) want = 1024;
    n = realloc(b->p, want);
    if (!n) return -1;
    b->p = n;
    b->cap = want;
    return 0;
}

static int bb_append(bytebuf *b, const uint8_t *src, uint32_t n)
{
    if (b->len + n > b->cap) {
        uint32_t want = b->cap ? b->cap * 2 : 1024;
        while (want < b->len + n) want *= 2;
        if (bb_reserve(b, want) != 0) return -1;
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;
    return 0;
}

/* ---- the device ----------------------------------------------------------- */

struct coleco_cart {
    uint8_t window[COLMAP_WINDOW];
    uint8_t staged[COLMAP_WINDOW];

    /* The full image for the banked kinds; a flat image lives wholly in the
     * windows above. */
    uint8_t *image;
    uint32_t image_size;
    uint8_t *staged_image;
    uint32_t staged_image_size;

    bytebuf rx[2]; /* per-stream DBC push buffers: 0 = ROM, 1 = the .cfg */
    colmap_plan_t staged_plan;
    colmap_kind_t pending_hint;

    /* What read() serves: this process's copy of core1's serve state. */
    colmap_serve_t serve;
    const uint8_t *base;

    int mailbox_live;
    int have_staged;
    int staged_claims;
    int swap_armed;
    int debug;
};

/* fujimail's port interface takes no context argument, so the single
 * instance is reached through this. One cart slot, one cart. */
static coleco_cart *s_cart;

/* ---- the fujimail port ---------------------------------------------------- */

static void c_poke(unsigned offset, uint8_t value)
{
    coleco_cart *c = s_cart;
    if (!c) return;
    c->window[offset & (COLMAP_WINDOW - 1)] = value;
    /* A staged image that claims the mailbox must receive the same publishes,
     * or the client would boot into stale status pages. One that does not
     * claim it keeps its bytes pristine -- and the mailbox stays live on the
     * *current* window until the swap actually happens, so the client still
     * sees BOOT_READY. */
    if (c->have_staged && c->staged_claims)
        c->staged[offset & (COLMAP_WINDOW - 1)] = value;
}

static bool c_link_up(void) { return fujitcp_active(); }

static uint8_t c_stream_open(int stream, uint32_t size)
{
    coleco_cart *c = s_cart;
    uint8_t err;
    if (!c) return FN_BOOT_ERR_NOMAP;
    /* colmap_gate refuses a size no mapper can serve before a byte is
     * transferred, so a 512K MegaCart fails fast rather than after a minute
     * of pushing. */
    err = (stream == FN_STREAM_ROM) ? colmap_gate(size) : 0;
    if (err) return err;
    bb_clear(&c->rx[stream & 1]);
    if (size) bb_reserve(&c->rx[stream & 1], size);
    return 0;
}

static void c_stream_write(int stream, const uint8_t *chunk, unsigned len)
{
    coleco_cart *c = s_cart;
    if (!c) return;
    bb_append(&c->rx[stream & 1], chunk, (uint32_t)len);
}

static uint8_t c_stream_close(int stream, uint32_t got, bool aborted)
{
    coleco_cart *c = s_cart;
    bytebuf *v;
    colmap_plan_t plan;
    colmap_kind_t hint;

    (void)got; /* v->len is the byte count that actually landed */
    if (!c) return FN_BOOT_ERR_NOMAP;
    v = &c->rx[stream & 1];

    if (stream == FN_STREAM_CFG) {
        /* The mapper hint. Three Activision cartridges are 64K and two Opcode
         * SGC cartridges are 128K, and both sizes are also MegaCart sizes --
         * nothing in the image can tell them apart, so a one-line .cfg has
         * to. */
        c->pending_hint = (aborted || v->len == 0)
            ? COLMAP_KIND_AUTO
            : colmap_parse_cfg((const char *)v->p, v->len);
        bb_clear(v);
        return 0;
    }
    if (stream != FN_STREAM_ROM) {
        bb_clear(v);
        return 0;
    }

    /* Consume the hint here whatever happens next: the ESP32 sends a .cfg
     * only when the file exists, so a mount with no sibling sends nothing at
     * all and would otherwise inherit the previous mount's mapper. */
    hint = c->pending_hint;
    c->pending_hint = COLMAP_KIND_AUTO;

    if (aborted) {
        bb_clear(v);
        return 0;
    }
    if (v->len == 0 ||
        colmap_plan(v->p, v->len, hint, &plan) != COLMAP_OK) {
        bb_clear(v);
        return FN_BOOT_ERR_NOMAP;
    }

    free(c->staged_image);
    c->staged_image = NULL;
    c->staged_image_size = 0;

    if (colmap_serves_window(&plan)) {
        colmap_apply(v->p, &plan, c->staged);
        bb_clear(v);
    } else {
        /* Hand the buffer over rather than copying it: a banked image is up
         * to 128K and is served from here for the rest of the session. */
        c->staged_image = v->p;
        c->staged_image_size = v->len;
        v->p = NULL;
        v->len = v->cap = 0;
        memcpy(c->staged, c->staged_image, sizeof c->staged);
    }
    c->staged_plan = plan;
    c->staged_claims = plan.mailbox_ok;
    c->have_staged = 1;
    if (c->debug)
        fprintf(stderr, "fujinet: staged a %u-byte image as %s\n",
                (unsigned)plan.size, colmap_kindname(plan.kind));
    return 0;
}

static void c_arm_swap(void)
{
    coleco_cart *c = s_cart;
    if (c && c->have_staged) c->swap_armed = 1;
}

static void c_on_txn(const fujimail_txn_t *t)
{
    char txt[40];
    unsigned k, m = 0;
    for (k = 0; k < t->rxlen && m < sizeof txt - 1; k++) {
        uint8_t ch = t->rx[k];
        if (ch == 0) break;
        txt[m++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
    }
    txt[m] = '\0';
    fprintf(stderr,
            "fujinet: dev=%02X cmd=%02X nparam=%u txlen=%u seq=%u"
            " -> err=%d reply=%02X rxlen=%u%s%s%s\n",
            t->device, t->command, t->nparam, t->txlen, t->seq,
            t->status, t->reply_cmd, t->rxlen,
            m ? " \"" : "", txt, m ? "\"" : "");
}

static void c_on_dbc(fujimail_dbc_ev_t ev, int stream, uint32_t expect,
                     unsigned got, bool aborted)
{
    if (ev == FUJIMAIL_DBC_OPEN)
        fprintf(stderr, "fujinet: DBC open stream=%d size=%u\n", stream,
                (unsigned)expect);
    else
        fprintf(stderr, "fujinet: DBC close stream=%d got=%u%s\n", stream, got,
                aborted ? " ABORTED" : "");
}

/* wait_link_ms is NULL: fujitcp's round trip is synchronous, so there is
 * nothing to pump while waiting. bootsel is NULL: there is no RP2040 to
 * reboot into a bootloader here. */
static const fujimail_port_t port_quiet = {
    c_poke, c_link_up, fujitcp_transact, fujitcp_send_bare,
    c_stream_open, c_stream_write, c_stream_close, c_arm_swap,
    NULL, NULL, NULL, NULL
};

static const fujimail_port_t port_debug = {
    c_poke, c_link_up, fujitcp_transact, fujitcp_send_bare,
    c_stream_open, c_stream_write, c_stream_close, c_arm_swap,
    NULL, NULL, c_on_txn, c_on_dbc
};

/* ---- serving -------------------------------------------------------------- */

/* Point the serve state at the live image -- this process's copy of core1's
 * swap. image must already hold the full image for the banked kinds, and
 * window the flat one. */
static void apply_serving(coleco_cart *c, const colmap_plan_t *plan)
{
    colmap_serve_reset(plan, &c->serve);
    c->base = colmap_serves_window(plan) ? c->window : c->image;
}

static void do_swap(coleco_cart *c)
{
    memcpy(c->window, c->staged, sizeof c->window);
    free(c->image);
    c->image = c->staged_image;
    c->image_size = c->staged_image_size;
    c->staged_image = NULL;
    c->staged_image_size = 0;

    apply_serving(c, &c->staged_plan);
    c->swap_armed = 0;
    c->have_staged = 0;
    c->mailbox_live = c->staged_claims;
    /* Repaint so the incoming client sees a cleared status page and, above
     * all, lastseq reset -- otherwise its first transaction would reuse a
     * sequence number the cartridge has already acknowledged. */
    if (c->mailbox_live)
        fujimail_paint();
    fprintf(stderr,
            "fujinet: swapped in the staged image (%s); mailbox %s for this "
            "session\n",
            colmap_kindname(c->staged_plan.kind),
            c->mailbox_live ? "kept" : "disabled");
}

/* ---- the adamcore_cart_ops vtable ----------------------------------------- */

static uint8_t cart_read(void *ud, uint16_t off, int commit)
{
    coleco_cart *c = ud;
    int32_t addr;
    uint8_t data;

    off &= (COLMAP_WINDOW - 1);

    /* A mapper that stops driving reads back as an undriven bus. This is
     * stricter than MAME's own devices -- sgc.cpp happily returns flash data
     * from $FFFC -- and that is the point: model what the silicon does. */
    if (colmap_tristate(&c->serve, off))
        return 0xFF;

    addr = colmap_serve(&c->serve, off, commit ? true : false);
    data = (addr < 0) ? 0xFF : c->base[addr];

    /* Hotspot side effects: never for the debugger (commit == 0), never once
     * the mailbox is dead. The swap is handled here rather than inside
     * fujimail because it has to happen inline in whatever serves the bus --
     * the RP2040 does it in core1's loop for the same reason. */
    if (commit && off >= FN_H_REGSEL) {
        if (off == FN_H_REGSEL + FN_HOT_SWAP) {
            if (c->swap_armed) do_swap(c);
        } else if (c->mailbox_live) {
            fujimail_read_hotspot(off);
        }
    }
    return data;
}

static void cart_write(void *ud, uint16_t off, uint8_t v)
{
    coleco_cart *c = ud;
    off &= (COLMAP_WINDOW - 1);
    /* On the real cartridge port a write is indistinguishable from a read --
     * the chip selects are qualified by /MREQ and /RFSH only -- so anything a
     * read would have done, a write does too. That is exactly how Activision
     * cartridges switch banks. The data byte matters only to the SGC.
     *
     * The mailbox is deliberately NOT driven from here: it rides the read
     * path alone, and adamcore already guarantees this is a real bus cycle
     * (a debugger poke never reaches a cart device). */
    colmap_serve_write(&c->serve, off, v);
    (void)colmap_serve(&c->serve, off, true);
}

static void cart_reset(void *ud)
{
    (void)ud;
    /* Power-on only, and adamcore calls it once at install. Nothing to do:
     * the constructor has already built the window, and a console reset must
     * leave the cartridge exactly as it is -- on hardware the cartridge edge
     * carries no reset line at all, which is also what lets FN_R_ACKSEQ
     * survive a reset the client does not. */
}

static const adamcore_cart_ops ops = { cart_read, cart_write, cart_reset };

const adamcore_cart_ops *coleco_cart_ops(void) { return &ops; }

/* ---- lifecycle ------------------------------------------------------------ */

coleco_cart *coleco_cart_create(const uint8_t *image, uint32_t size,
                                const char *hostport)
{
    coleco_cart *c;
    colmap_plan_t plan;

    if (s_cart) return NULL; /* one cart slot, one cart */

    c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->debug = getenv("FUJINET_DEBUG") != NULL;
    c->pending_hint = COLMAP_KIND_AUTO;
    memset(c->window, 0xFF, sizeof c->window);
    memset(c->staged, 0xFF, sizeof c->staged);

    /* With no cartridge, serve the baked-in CONFIG client -- the same image
     * the RP2040 bakes in, and the reason the app boots to something useful
     * with no media at all. */
    if (!image || size == 0) {
        image = _configrom;
        size = FUJI_CONFIGROM_SIZE;
    }

    if (colmap_plan(image, size, COLMAP_KIND_AUTO, &plan) == COLMAP_OK) {
        if (colmap_serves_window(&plan)) {
            colmap_apply(image, &plan, c->window);
        } else {
            c->image = malloc(size);
            if (!c->image) { free(c); return NULL; }
            memcpy(c->image, image, size);
            c->image_size = size;
        }
        apply_serving(c, &plan);
        c->mailbox_live = plan.mailbox_ok;
    } else {
        colmap_plan_t flat;
        flat.size = COLMAP_WINDOW;
        flat.kind = COLMAP_FLAT;
        flat.mailbox_ok = false;
        flat.nbanks = 0;
        apply_serving(c, &flat);
        c->mailbox_live = 0;
    }

    s_cart = c;

    if (c->mailbox_live) {
        fujimail_init(c->debug ? &port_debug : &port_quiet);
        /* Failing to connect is not fatal: FujiNet may still be starting, and
         * fujitcp retries. The client sees the link-down status bit and says
         * so, which is better than refusing to boot. */
        fujitcp_init(hostport);
        fujimail_paint();
    } else {
        fprintf(stderr, "fujinet: image carries no claim signature; running "
                        "with the mailbox dead\n");
    }
    return c;
}

void coleco_cart_destroy(coleco_cart *c)
{
    if (!c) return;
    if (s_cart == c) {
        fujitcp_close();
        s_cart = NULL;
    }
    bb_free(&c->rx[0]);
    bb_free(&c->rx[1]);
    free(c->image);
    free(c->staged_image);
    free(c);
}

int coleco_cart_mailbox_live(const coleco_cart *c)
{
    return c ? c->mailbox_live : 0;
}

int coleco_cart_link_up(const coleco_cart *c)
{
    (void)c;
    return fujitcp_active() ? 1 : 0;
}

colmap_kind_t coleco_cart_kind(const coleco_cart *c)
{
    return c ? c->serve.kind : COLMAP_FLAT;
}
