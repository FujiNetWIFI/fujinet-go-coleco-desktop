/*
 * fujitrace_test -- the debugger's FujiNet trace decoder, on synthetic bytes.
 *
 * The cartridge hands the trace SLIP frames in both directions; this feeds
 * it the same shapes by hand: whole frames, frames in one-byte pieces,
 * escapes, a reply cut short by the next request, frames too short to carry
 * a header, polling that must fold into one counted entry, and a ring that
 * has to drop its oldest. No ROM, no network.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "fujitrace.h"

#define TO_FUJINET 1 /* fujitrace_feed's dir */
#define TO_COLECO  0

static int failures;
static fujitrace t; /* static: too big for a stack */
static coleco_fuji_packet p[FUJITRACE_MAX];

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok " : "FAIL", what);
    if (!ok) failures++;
}

/* A FujiBus packet with no params, SLIP-framed: what the wire carries. */
static size_t frame(uint8_t dev, uint8_t cmd, const uint8_t *data,
                    size_t len, uint8_t *out)
{
    uint8_t pkt[1024];
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

static void feed(int dir, uint8_t dev, uint8_t cmd, const uint8_t *data,
                 size_t len)
{
    uint8_t f[2100];
    size_t n = frame(dev, cmd, data, len, f);
    fujitrace_feed(&t, dir, f, n);
}

static int read_all(void) { return fujitrace_read(&t, p, FUJITRACE_MAX); }

int main(void)
{
    static const uint8_t getcfg[] = {0xC0, 0x70, 0xC4, 0x06, 0x00,
                                     0x3B, 0x00, 0xC0};
    uint8_t f[2100], data[600];
    size_t n, i;
    int k;

    printf("empty:\n");
    check("a new trace holds nothing", read_all() == 0);
    check("...and has counted nothing",
          t.packets[0] == 0 && t.packets[1] == 0 && t.bytes[0] == 0);

    printf("a request:\n");
    fujitrace_feed(&t, TO_FUJINET, getcfg, sizeof getcfg);
    k = read_all();
    check("one packet", k == 1);
    check("going to FujiNet", p[0].to_host == 0);
    check("device, command and length from the header",
          p[0].device == 0x70 && p[0].command == 0xC4 && p[0].length == 6);
    check("six bytes, SLIP framing removed",
          p[0].nbytes == 6 && p[0].data[0] == 0x70 && p[0].data[4] == 0x3B);
    check("the text names the command",
          strcmp(p[0].text, "ColecoVision \xe2\x86\x92 FujiNet 70 C4 GET "
                            "ADAPTERCONFIG EXTENDED (6 bytes)") == 0);
    printf("      %s\n", p[0].text);

    printf("a reply in one-byte pieces, with escapes:\n");
    data[0] = 0xC0;
    data[1] = 0xDB;
    data[2] = 0x41;
    n = frame(0x70, 0x06, data, 3, f);
    for (i = 0; i < n; i++) fujitrace_feed(&t, TO_COLECO, f + i, 1);
    k = read_all();
    check("a second packet", k == 2);
    check("coming from FujiNet", p[1].to_host == 1);
    check("escapes undone",
          p[1].nbytes == 9 && p[1].data[6] == 0xC0 && p[1].data[7] == 0xDB &&
          p[1].data[8] == 0x41);
    check("the length field agrees", p[1].length == 9);
    check("named as the ACK it is",
          strcmp(p[1].text, "FujiNet \xe2\x86\x92 ColecoVision 70 06 ACK "
                            "(9 bytes)") == 0);
    check("serials rise", p[1].serial > p[0].serial);
    check("counted both ways",
          t.packets[0] == 1 && t.packets[1] == 1 && t.bytes[0] == 6 &&
          t.bytes[1] == 9);

    printf("a reply cut short by the next request:\n");
    fujitrace_clear(&t);
    n = frame(0x70, 0x06, (const uint8_t *)"partial", 7, f);
    fujitrace_feed(&t, TO_COLECO, f, n / 2); /* no closing END */
    fujitrace_feed(&t, TO_FUJINET, getcfg, sizeof getcfg);
    feed(TO_COLECO, 0x70, 0x15, NULL, 0); /* a whole NAK */
    k = read_all();
    check("the request and the new reply, nothing glued", k == 2);
    check("the reply is the NAK alone",
          k == 2 && p[1].to_host == 1 && p[1].command == 0x15 &&
          p[1].nbytes == 6);

    printf("a frame too short for a header:\n");
    fujitrace_clear(&t);
    {
        static const uint8_t runt[] = {0xC0, 0x01, 0x02, 0xC0};
        fujitrace_feed(&t, TO_COLECO, runt, sizeof runt);
    }
    k = read_all();
    check("still recorded", k == 1 && p[0].nbytes == 2);
    check("with no header fields",
          p[0].device == -1 && p[0].command == -1 && p[0].length == -1);
    check("and a plain byte count",
          strcmp(p[0].text, "FujiNet \xe2\x86\x92 ColecoVision 2 bytes") == 0);
    check("back-to-back ENDs make no empty packets",
          (fujitrace_feed(&t, TO_COLECO, (const uint8_t *)"\xC0\xC0\xC0", 3),
           read_all() == 1));

    printf("a long frame:\n");
    fujitrace_clear(&t);
    for (i = 0; i < 500; i++) data[i] = (uint8_t)i;
    feed(TO_COLECO, 0xFF, 0x57, data, 500); /* a DBC push block */
    k = read_all();
    check("the whole length is counted", k == 1 && p[0].nbytes == 506);
    check("the first 64 bytes are kept",
          p[0].data[6] == 0 && p[0].data[63] == 57);
    check("named WRITE", strstr(p[0].text, "FF 57 WRITE (506 bytes)") != NULL);

    printf("polling folds into one counted exchange:\n");
    fujitrace_clear(&t);
    for (k = 0; k < 3; k++) {
        fujitrace_feed(&t, TO_FUJINET, getcfg, sizeof getcfg);
        feed(TO_COLECO, 0x70, 0x06, (const uint8_t *)"cfg", 3);
    }
    k = read_all();
    check("three round trips, two entries", k == 2);
    check("each counted three times", p[0].repeat == 3 && p[1].repeat == 3);
    check("the count is in the text",
          strstr(p[0].text, "(6 bytes) \xc3\x97" "3") != NULL &&
          strstr(p[1].text, "(9 bytes) \xc3\x97" "3") != NULL);
    printf("      %s\n      %s\n", p[0].text, p[1].text);
    check("the packet counters still see all six",
          t.packets[0] == 3 && t.packets[1] == 3);
    {
        uint64_t last = p[1].serial;
        fujitrace_feed(&t, TO_FUJINET, getcfg, sizeof getcfg);
        feed(TO_COLECO, 0x70, 0x06, (const uint8_t *)"cfg", 3);
        read_all();
        check("a fold gives the newest entry a new serial",
              p[1].serial != last && p[1].repeat == 4);
    }
    /* The same request with a different answer is not the same exchange. */
    fujitrace_feed(&t, TO_FUJINET, getcfg, sizeof getcfg);
    feed(TO_COLECO, 0x70, 0x06, (const uint8_t *)"new", 3);
    k = read_all();
    check("a changed reply starts new entries", k == 4);
    check("...and the earlier pair keeps its count",
          p[0].repeat == 4 && p[1].repeat == 4);
    check("...the new pair counts once", p[2].repeat == 1 && p[3].repeat == 1);
    check("...with the request first again", p[2].to_host == 0 &&
          p[3].to_host == 1 && p[3].data[6] == 'n');

    printf("half an exchange again is not a repeat yet:\n");
    fujitrace_clear(&t);
    fujitrace_feed(&t, TO_FUJINET, getcfg, sizeof getcfg);
    feed(TO_COLECO, 0x70, 0x06, NULL, 0);
    fujitrace_feed(&t, TO_FUJINET, getcfg, sizeof getcfg); /* no reply yet */
    k = read_all();
    check("request, reply, request: three entries, none counted twice",
          k == 3 && p[0].repeat == 1 && p[1].repeat == 1 && p[2].repeat == 1);
    feed(TO_COLECO, 0x70, 0x06, NULL, 0);
    k = read_all();
    check("the reply folds the pair", k == 2 && p[0].repeat == 2 &&
                                          p[1].repeat == 2);

    printf("a single packet repeated:\n");
    fujitrace_clear(&t);
    for (k = 0; k < 5; k++)
        fujitrace_feed(&t, TO_FUJINET, getcfg, sizeof getcfg);
    k = read_all();
    check("folds to one entry of five", k == 1 && p[0].repeat == 5);
    feed(TO_COLECO, 0x70, 0x06, NULL, 0);
    fujitrace_feed(&t, TO_FUJINET, getcfg, sizeof getcfg);
    feed(TO_COLECO, 0x70, 0x15, NULL, 0); /* not the ACK: no fold */
    k = read_all();
    check("A x5, ACK, A, NAK stay four entries", k == 4);
    check("...with the counts they had",
          p[0].repeat == 5 && p[1].repeat == 1 && p[2].repeat == 1 &&
          p[3].repeat == 1 && p[3].command == 0x15);

    printf("the ring:\n");
    fujitrace_clear(&t);
    for (i = 0; i < FUJITRACE_MAX + 88; i++) {
        data[0] = (uint8_t)i;
        data[1] = (uint8_t)(i >> 8);
        feed(TO_FUJINET, 0x70, 0xF4, data, 2);
    }
    k = read_all();
    check("keeps the newest 512", k == FUJITRACE_MAX);
    check("oldest first",
          p[0].data[6] == (uint8_t)88 &&
          p[FUJITRACE_MAX - 1].data[6] == (uint8_t)(FUJITRACE_MAX + 87));
    {
        int rising = 1;
        for (k = 1; k < FUJITRACE_MAX; k++)
            if (p[k].serial <= p[k - 1].serial) rising = 0;
        check("serials rise across the wrap", rising);
    }
    check("a short read takes the newest",
          fujitrace_read(&t, p, 3) == 3 &&
          p[2].data[6] == (uint8_t)(FUJITRACE_MAX + 87));

    printf("clear:\n");
    fujitrace_feed(&t, TO_FUJINET, getcfg, 3); /* C0 70 C4: half a frame */
    fujitrace_clear(&t);
    check("empties the ring", read_all() == 0);
    fujitrace_feed(&t, TO_FUJINET, getcfg + 3, sizeof getcfg - 3);
    k = read_all();
    check("but keeps a frame in progress",
          k == 1 && p[0].command == 0xC4 && p[0].nbytes == 6);

    printf("names:\n");
    check("F4 is READ HOST SLOTS",
          strcmp(fujitrace_command_name(0xF4), "READ HOST SLOTS") == 0);
    check("an unnamed id is empty", fujitrace_command_name(0x99)[0] == '\0');
    fujitrace_clear(&t);
    feed(TO_FUJINET, 0x70, 0x99, NULL, 0);
    read_all();
    check("...and leaves no gap in the text",
          strstr(p[0].text, "70 99 (6 bytes)") != NULL);

    if (failures) {
        fprintf(stderr, "\nfujitrace_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("\nfujitrace_test: PASS\n");
    return 0;
}
