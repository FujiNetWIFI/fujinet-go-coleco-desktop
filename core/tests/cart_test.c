/*
 * cart_test -- the FujiNet cartridge device, driven through a real Z80.
 *
 * The firmware's own host_test/test_fujimail.c already exercises the
 * protocol against a stub port, and MAME's device covers the same ground
 * from the other side. What neither covers is THIS layer: the transposed
 * device (core/coleco/fujinet_cart.c) plugged into adamcore's bus, where the
 * hotspot reads come from an actual instruction stream rather than a test
 * harness calling fujimail_read_hotspot() directly.
 *
 * That distinction is the whole point. Every mailbox transaction on real
 * hardware is a pair of cartridge READS whose ADDRESS carries the payload --
 * reading $FD05 arms register 5, reading $FE2A sets it to 0x2A. If adamcore
 * dispatched a read twice, or fired one for a debugger peek, or lost the
 * commit flag, everything below would still pass in the firmware's tests and
 * fail here.
 *
 * Needs no ROMs and no network: with nothing listening the link is simply
 * down, which is itself one of the states worth asserting. On POSIX a
 * loopback stand-in for FujiNet then checks the debugger's trace tap against
 * the bytes actually on the wire.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "adamcore.h"
#include "adamcore_debug.h"
#include "fuji_mailbox.h"
#include "fujinet_cart.h"
#include "fujitrace.h"

static uint8_t os7[ADAMCORE_OS7_ROM_SIZE];
static int failures;

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok " : "FAIL", what);
    if (!ok) failures++;
}

/* ---- driving the machine -------------------------------------------------- */

/* The program buffer lives in the console's real 1K at $6000. */
#define PROG_BASE 0x6000
static uint8_t prog[512];
static int prog_len;

static void emit(uint8_t b) { if (prog_len < (int)sizeof prog) prog[prog_len++] = b; }

/* LD A,(nn) -- one cartridge read of nn, which is how every mailbox touch is
 * made. The value read is discarded; the ADDRESS is the message. */
static void emit_read(uint16_t addr)
{
    emit(0x3A);
    emit((uint8_t)(addr & 0xFF));
    emit((uint8_t)(addr >> 8));
}

/* The client's register write: arm the register, then supply the value. */
static void emit_regwr(uint8_t reg, uint8_t val)
{
    emit_read((uint16_t)(0xFD00 + reg));
    emit_read((uint16_t)(0xFE00 + val));
}

static void prog_begin(void) { prog_len = 0; }

/* Load the program into RAM and run it to completion. Instruction count is
 * derived from the program, so a HALT at the end is not needed. */
static void prog_run(adamcore *c)
{
    adamcore_z80_regs r;
    int i, ninstr = 0;

    for (i = 0; i < prog_len; i++)
        adamcore_poke(c, (uint16_t)(PROG_BASE + i), prog[i]);
    for (i = 0; i < prog_len; i += 3)
        ninstr++;

    memset(&r, 0, sizeof r);
    adamcore_get_regs(c, &r);
    r.pc = PROG_BASE;
    adamcore_set_regs(c, &r);
    adamcore_debug_run(c, (uint32_t)ninstr, 0, NULL);
}

/* The debugger's trace tap: count what it is handed. */
static size_t traced;

static void count_trace(void *user, int dir, const uint8_t *buf, size_t len)
{
    (void)user;
    (void)dir;
    (void)buf;
    traced += len;
}

#ifndef _WIN32
/* ---- a stand-in FujiNet on loopback ---------------------------------------
 * It answers one request the way MediaTypeROM::mount() answers MOUNT_IMAGE:
 * a pushed stream (OPEN, WRITE, CLOSE, each waiting for its ACK) and then the
 * reply. Both ends keep every byte, so the trace can be held to the wire. */

typedef struct {
    uint8_t b[4096];
    size_t n;
} wire;

static wire traced_wire[2]; /* by the trace's dir */
static fujitrace traced_ft;

static void keep_trace(void *user, int dir, const uint8_t *buf, size_t len)
{
    wire *w = &traced_wire[dir ? 1 : 0];
    (void)user;
    if (w->n + len <= sizeof w->b) {
        memcpy(w->b + w->n, buf, len);
        w->n += len;
    }
    fujitrace_feed(&traced_ft, dir, buf, len);
}

/* A FujiBus packet with no params, SLIP-framed, as FujiNet sends one. */
static size_t slip_frame(uint8_t dev, uint8_t cmd, const uint8_t *data,
                         size_t len, uint8_t *out)
{
    uint8_t pkt[600];
    uint16_t ck = 0;
    size_t n = 6 + len, i, o = 0;

    pkt[0] = dev;
    pkt[1] = cmd;
    pkt[2] = (uint8_t)(n & 0xFF);
    pkt[3] = (uint8_t)(n >> 8);
    pkt[4] = 0;
    pkt[5] = 0;
    memcpy(pkt + 6, data, len);
    for (i = 0; i < n; i++) {
        ck = (uint16_t)(ck + pkt[i]);
        ck = (uint16_t)((ck >> 8) + (ck & 0xFF));
    }
    pkt[4] = (uint8_t)ck;
    out[o++] = 0xC0;
    for (i = 0; i < n; i++) {
        if (pkt[i] == 0xC0 || pkt[i] == 0xDB) {
            out[o++] = 0xDB;
            out[o++] = pkt[i] == 0xC0 ? 0xDC : 0xDD;
        } else {
            out[o++] = pkt[i];
        }
    }
    out[o++] = 0xC0;
    return o;
}

typedef struct {
    int listen_fd, fd;
    wire got, sent; /* the stand-in's side of the wire */
    int ok;
} fake_fujinet;

static int fake_read_frame(fake_fujinet *f)
{
    int ends = 0;
    uint8_t c;
    while (ends < 2) {
        if (recv(f->fd, &c, 1, 0) != 1) return -1;
        if (f->got.n < sizeof f->got.b) f->got.b[f->got.n++] = c;
        if (c == 0xC0) ends++;
    }
    return 0;
}

static void fake_send(fake_fujinet *f, uint8_t dev, uint8_t cmd,
                      const uint8_t *data, size_t len)
{
    uint8_t frame[1300];
    size_t n = slip_frame(dev, cmd, data, len, frame);
    memcpy(f->sent.b + f->sent.n, frame, n);
    f->sent.n += n;
    send(f->fd, frame, n, 0);
}

static void *fake_main(void *arg)
{
    /* the .cfg stream, three bytes, two of them needing SLIP escapes */
    static const uint8_t open_hdr[5] = {1, 3, 0, 0, 0};
    static const uint8_t cfg[3] = {0xC0, 0xDB, 'x'};
    static const uint8_t reply[4] = {0xDB, 0xC0, 0x00, 0x42};
    fake_fujinet *f = arg;

    f->fd = accept(f->listen_fd, NULL, NULL);
    if (f->fd < 0 || fake_read_frame(f) != 0) return NULL;
    fake_send(f, 0xFF, 0x4F, open_hdr, sizeof open_hdr);
    if (fake_read_frame(f) != 0) return NULL;
    fake_send(f, 0xFF, 0x57, cfg, sizeof cfg);
    if (fake_read_frame(f) != 0) return NULL;
    fake_send(f, 0xFF, 0x43, NULL, 0);
    if (fake_read_frame(f) != 0) return NULL;
    fake_send(f, 0x70, 0x06, reply, sizeof reply);
    f->ok = 1;
    return NULL;
}
#endif

/* Read a status byte the way a debugger would -- deliberately NOT through the
 * bus, so observing the mailbox cannot perturb it. */
static uint8_t status(adamcore *c, uint16_t addr)
{
    return adamcore_peek(c, addr);
}

int main(void)
{
    adamcore_config cfg;
    adamcore *c;
    coleco_cart *cart;
    uint8_t seq, ackseq_before, err;

    memset(os7, 0x00, sizeof os7);
    memset(&cfg, 0, sizeof cfg);
    cfg.os7_rom_data = os7;
    cfg.start_machine = ADAMCORE_MACHINE_CV;
    cfg.audio_rate = 44100;

    c = adamcore_create(&cfg);
    if (!c) { fprintf(stderr, "adamcore_create failed\n"); return 1; }

    /* No image: the device serves the baked-in CONFIG client, exactly as the
     * RP2040 cartridge does with nothing staged. Point it at a port nothing
     * is listening on -- the link-down path is a state worth testing, and it
     * keeps the test hermetic. */
    cart = coleco_cart_create(NULL, 0, "127.0.0.1:1");
    if (!cart) { fprintf(stderr, "coleco_cart_create failed\n"); return 1; }
    adamcore_set_cart_ops(c, coleco_cart_ops(), cart);
    coleco_cart_set_trace(count_trace, NULL);

    printf("the cartridge answers:\n");
    check("the mailbox is live on the CONFIG client",
          coleco_cart_mailbox_live(cart));
    check("'F' 'N' presence magic at $FC09/$FC0A",
          status(c, 0xFC09) == 'F' && status(c, 0xFC0A) == 'N');
    check("protocol version reads 1", status(c, 0xFC0B) == FN_PROTO_VER);
    check("the \"FUJI\" claim signature is at $FCFC",
          status(c, 0xFCFC) == 'F' && status(c, 0xFCFD) == 'U' &&
          status(c, 0xFCFE) == 'J' && status(c, 0xFCFF) == 'I');
    check("the client ROM is served below $F800",
          status(c, 0x8000) != 0xFF || status(c, 0x8001) != 0xFF);

    /* ---- a transaction, launched entirely from the instruction stream ----- */
    printf("a transaction driven by real cartridge reads:\n");
    ackseq_before = status(c, 0xFC00);
    /* The client derives its sequence number from what the cartridge last
     * acknowledged -- never a local counter, so a console reset cannot replay
     * a number already answered. 0 is reserved. */
    seq = (uint8_t)(ackseq_before + 1);
    if (seq == 0) seq = 1;

    prog_begin();
    emit_regwr(FN_REG_DATA_RST, 0);
    emit_regwr(FN_REG_DEVICE, 0x70);   /* the FujiNet device */
    emit_regwr(FN_REG_CMD, 0xC4);      /* GET_ADAPTERCONFIG_EXTENDED */
    emit_regwr(FN_REG_NPARAM, 0);
    emit_regwr(FN_REG_SEQ, seq);       /* launches it */
    prog_run(c);

    check("ACKSEQ echoes the sequence number we sent",
          status(c, 0xFC00) == seq);
    err = status(c, 0xFC02);
    check("the error byte reports no link (nothing is listening)",
          err == FN_ERR_NOLINK);
    check("the link-up status bit is clear",
          (status(c, 0xFC01) & FN_R_STATUS_LINK) == 0);
    check("coleco_cart_link_up() agrees", !coleco_cart_link_up(cart));
    check("the trace saw nothing: no byte reached the wire", traced == 0);

    /* ---- the stray-read defence ------------------------------------------ */
    printf("stray reads cannot disturb it:\n");
    {
        uint8_t ack = status(c, 0xFC00);
        uint8_t block[0x300];
        /* A debugger sweeping the three hotspot pages. Through the bus this
         * would arm registers, append TX bytes and -- at $FDFE -- attempt the
         * ROM swap. Through peek it must do nothing at all. */
        adamcore_peek_block(c, 0xFD00, block, sizeof block);
        check("a debugger sweep of $FD00-$FFFF leaves ACKSEQ alone",
              status(c, 0xFC00) == ack);
        check("...and the error byte alone", status(c, 0xFC02) == err);
    }
    {
        /* An unpaired REGDATA read is inert: REGSEL disarms after a single
         * use, which is what makes the vblank NMI landing between the pair
         * harmless. Fire a lone REGDATA at the SEQ value, then check no
         * transaction ran. */
        uint8_t ack = status(c, 0xFC00);
        prog_begin();
        emit_read((uint16_t)(0xFE00 + ((seq + 1) & 0xFF)));
        prog_run(c);
        check("a REGDATA read with no preceding REGSEL launches nothing",
              status(c, 0xFC00) == ack);
    }
    {
        /* Re-sending the SAME sequence number must not relaunch: retry is not
         * replay. Observable here because a relaunch would repaint the status
         * page, but the gate is SEQ != lastseq. */
        prog_begin();
        emit_regwr(FN_REG_SEQ, seq);
        prog_run(c);
        check("re-sending the same SEQ does not relaunch",
              status(c, 0xFC00) == seq);
    }
    {
        /* A fresh sequence number does launch again. */
        uint8_t next = (uint8_t)(seq + 1);
        if (next == 0) next = 1;
        prog_begin();
        emit_regwr(FN_REG_SEQ, next);
        prog_run(c);
        check("a new SEQ launches a new transaction",
              status(c, 0xFC00) == next);
    }

    coleco_cart_set_trace(NULL, NULL);
    adamcore_set_cart_ops(c, NULL, NULL);
    coleco_cart_destroy(cart);
    adamcore_destroy(c);

    /* ---- an image with no claim signature runs with the mailbox dead ------ */
    printf("an unsigned image runs as a plain cartridge:\n");
    {
        static uint8_t plain[0x8000];
        adamcore *c2 = adamcore_create(&cfg);
        coleco_cart *cart2;
        memset(plain, 0x00, sizeof plain);
        plain[0] = 0xAA; plain[1] = 0x55; /* a valid ColecoVision header */
        plain[0x100] = 0x42;
        cart2 = coleco_cart_create(plain, sizeof plain, "127.0.0.1:1");
        if (!c2 || !cart2) { fprintf(stderr, "second create failed\n"); return 1; }
        adamcore_set_cart_ops(c2, coleco_cart_ops(), cart2);
        check("the mailbox is dead without the claim signature",
              !coleco_cart_mailbox_live(cart2));
        check("no 'F' 'N' magic is painted",
              !(adamcore_peek(c2, 0xFC09) == 'F' &&
                adamcore_peek(c2, 0xFC0A) == 'N'));
        check("the image is served verbatim",
              adamcore_peek(c2, 0x8000) == 0xAA &&
              adamcore_peek(c2, 0x8100) == 0x42);
        adamcore_set_cart_ops(c2, NULL, NULL);
        coleco_cart_destroy(cart2);
        adamcore_destroy(c2);
    }

#ifndef _WIN32
    /* ---- the debugger's trace, held to the wire --------------------------- */
    printf("the trace matches the wire, pushes and all:\n");
    {
        static const struct { int to_host, dev, cmd; } want[] = {
            {0, 0x70, 0xC4}, {1, 0xFF, 0x4F}, {0, 0xFF, 0x06},
            {1, 0xFF, 0x57}, {0, 0xFF, 0x06}, {1, 0xFF, 0x43},
            {0, 0xFF, 0x06}, {1, 0x70, 0x06},
        };
        coleco_fuji_packet pk[16];
        fake_fujinet f;
        pthread_t th;
        struct sockaddr_in a;
        socklen_t alen = sizeof a;
        char hostport[32];
        adamcore *c3;
        coleco_cart *cart3;
        int n, i, order = 1;

        memset(&f, 0, sizeof f);
        f.fd = -1;
        f.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (f.listen_fd < 0 ||
            bind(f.listen_fd, (struct sockaddr *)&a, sizeof a) != 0 ||
            listen(f.listen_fd, 1) != 0 ||
            getsockname(f.listen_fd, (struct sockaddr *)&a, &alen) != 0) {
            fprintf(stderr, "loopback listener failed\n");
            return 1;
        }
        snprintf(hostport, sizeof hostport, "127.0.0.1:%d", ntohs(a.sin_port));
        pthread_create(&th, NULL, fake_main, &f);

        c3 = adamcore_create(&cfg);
        cart3 = coleco_cart_create(NULL, 0, hostport);
        if (!c3 || !cart3) { fprintf(stderr, "third create failed\n"); return 1; }
        adamcore_set_cart_ops(c3, coleco_cart_ops(), cart3);
        coleco_cart_set_trace(keep_trace, NULL);

        seq = (uint8_t)(status(c3, 0xFC00) + 1);
        if (seq == 0) seq = 1;
        prog_begin();
        emit_regwr(FN_REG_DATA_RST, 0);
        emit_regwr(FN_REG_DEVICE, 0x70);
        emit_regwr(FN_REG_CMD, 0xC4);
        emit_regwr(FN_REG_NPARAM, 0);
        emit_regwr(FN_REG_SEQ, seq);
        prog_run(c3);
        pthread_join(th, NULL);

        check("the stand-in served the whole exchange", f.ok);
        check("the client saw the reply",
              status(c3, 0xFC00) == seq && status(c3, 0xFC02) == FN_ERR_OK &&
              status(c3, 0xFC03) == 0x06);
        check("traced as sent: byte for byte what FujiNet received",
              traced_wire[1].n == f.got.n &&
              memcmp(traced_wire[1].b, f.got.b, f.got.n) == 0);
        check("traced as received: byte for byte what FujiNet sent",
              traced_wire[0].n == f.sent.n &&
              memcmp(traced_wire[0].b, f.sent.b, f.sent.n) == 0);
        n = fujitrace_read(&traced_ft, pk, 16);
        for (i = 0; i < n && i < 8; i++)
            if (pk[i].to_host != want[i].to_host ||
                pk[i].device != want[i].dev || pk[i].command != want[i].cmd)
                order = 0;
        check("in the order they crossed: request, each push and its ACK, "
              "reply", n == 8 && order);
        for (i = 0; i < n; i++)
            printf("      %s\n", pk[i].text);

        coleco_cart_set_trace(NULL, NULL);
        adamcore_set_cart_ops(c3, NULL, NULL);
        coleco_cart_destroy(cart3);
        adamcore_destroy(c3);
        if (f.fd >= 0) close(f.fd);
        close(f.listen_fd);
    }
#endif

    if (failures) {
        fprintf(stderr, "\ncart_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("\ncart_test: PASS\n");
    return 0;
}
