/*
 * FujiNet trace: SLIP frames into FujiBus packets. See fujitrace.h.
 *
 * Consecutive identical exchanges fold into one entry with a count, so a
 * client polling the same request does not push everything else out of the
 * ring. An exchange is one packet repeated (A A A) or a request and its reply
 * repeated (A B A B). A repeated A stays an entry of its own until the B after
 * it folds both into the counts, so the log never claims a repeat that has
 * not happened.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fujitrace.h"

#include <stdio.h>
#include <string.h>

#include "fuji_names.h"

#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

const char *fujitrace_command_name(int id)
{
    size_t i;
    for (i = 0; i < sizeof fuji_command_names / sizeof fuji_command_names[0]; i++)
        if (fuji_command_names[i].id == id)
            return fuji_command_names[i].name;
    return "";
}

static uint32_t fnv1a(const uint8_t *p, int n)
{
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static int slot(const fujitrace *t, int back) /* 0: the newest */
{
    return (t->head + t->count - 1 - back) % FUJITRACE_MAX;
}

static void describe(coleco_fuji_packet *p)
{
    const char *who = p->to_host ? "FujiNet \xe2\x86\x92 ColecoVision"
                                 : "ColecoVision \xe2\x86\x92 FujiNet";
    char times[24] = "";
    if (p->repeat > 1)
        snprintf(times, sizeof times, " \xc3\x97%u", (unsigned)p->repeat);
    if (p->device >= 0) {
        const char *name = fujitrace_command_name(p->command);
        snprintf(p->text, sizeof p->text, "%s %02X %02X%s%s (%d bytes)%s", who,
                 p->device, p->command, *name ? " " : "", name, p->nbytes,
                 times);
    } else {
        snprintf(p->text, sizeof p->text, "%s %d bytes%s", who, p->nbytes,
                 times);
    }
}

static int same(const fujitrace *t, int s, const coleco_fuji_packet *p,
                uint32_t hash)
{
    const coleco_fuji_packet *q = &t->ring[s];
    int n = p->nbytes < (int)sizeof p->data ? p->nbytes : (int)sizeof p->data;
    return q->to_host == p->to_host && q->nbytes == p->nbytes &&
           t->hash[s] == hash && memcmp(q->data, p->data, (size_t)n) == 0;
}

static void bump(fujitrace *t, int s)
{
    t->ring[s].repeat++;
    t->ring[s].serial = ++t->serial; /* a reader sees the count change */
    describe(&t->ring[s]);
}

static void push(fujitrace *t, const coleco_fuji_packet *p, uint32_t hash)
{
    int s;
    if (t->count == FUJITRACE_MAX) {
        t->head = (t->head + 1) % FUJITRACE_MAX;
        t->count--;
    }
    t->count++;
    s = slot(t, 0);
    t->ring[s] = *p;
    t->ring[s].serial = ++t->serial;
    t->ring[s].repeat = 1;
    t->hash[s] = hash;
    describe(&t->ring[s]);
}

static void add(fujitrace *t, const coleco_fuji_packet *p, uint32_t hash)
{
    t->packets[p->to_host]++;
    t->bytes[p->to_host] += (uint64_t)p->nbytes;

    if (t->pending) {
        /* The newest entry repeats the one two back (A B A); this B
         * completes the exchange again. */
        t->pending = 0;
        if (same(t, slot(t, 1), p, hash)) {
            t->count--;
            bump(t, slot(t, 1));
            bump(t, slot(t, 0));
            return;
        }
    }
    if (t->count >= 1 && same(t, slot(t, 0), p, hash)) {
        bump(t, slot(t, 0));
        return;
    }
    push(t, p, hash);
    if (t->count >= 3) {
        const int a = slot(t, 0);
        t->pending = same(t, slot(t, 2), &t->ring[a], t->hash[a]) &&
                     t->ring[slot(t, 2)].repeat == t->ring[slot(t, 1)].repeat;
    }
}

static void frame_done(fujitrace *t, int to_host)
{
    const uint8_t *a = t->acc[to_host];
    const int n = t->acclen[to_host];
    coleco_fuji_packet p;

    memset(&p, 0, sizeof p);
    p.to_host = to_host;
    p.nbytes = n;
    memcpy(p.data, a, (size_t)(n < (int)sizeof p.data ? n : (int)sizeof p.data));
    p.device = n >= 6 ? a[0] : -1;
    p.command = n >= 6 ? a[1] : -1;
    p.length = n >= 6 ? (a[2] | (a[3] << 8)) : -1;
    add(t, &p, fnv1a(a, n));
    t->acclen[to_host] = 0;
}

void fujitrace_feed(fujitrace *t, int dir, const uint8_t *buf, size_t len)
{
    const int to_host = dir == 0 ? 1 : 0;
    size_t i;

    /* A request means any reply still in pieces never finished. */
    if (!to_host) {
        t->acclen[1] = 0;
        t->esc[1] = 0;
    }
    for (i = 0; i < len; i++) {
        uint8_t b = buf[i];
        if (b == SLIP_END) {
            if (t->acclen[to_host] > 0)
                frame_done(t, to_host);
            t->esc[to_host] = 0;
            continue;
        }
        if (t->esc[to_host]) {
            t->esc[to_host] = 0;
            b = b == SLIP_ESC_END ? SLIP_END : b == SLIP_ESC_ESC ? SLIP_ESC : b;
        } else if (b == SLIP_ESC) {
            t->esc[to_host] = 1;
            continue;
        }
        if (t->acclen[to_host] < FUJITRACE_FRAME_MAX)
            t->acc[to_host][t->acclen[to_host]++] = b;
    }
}

int fujitrace_read(const fujitrace *t, coleco_fuji_packet *out, int max)
{
    int n = t->count < max ? t->count : max, i;
    for (i = 0; i < n; i++)
        out[i] = t->ring[slot(t, n - 1 - i)];
    return n;
}

void fujitrace_clear(fujitrace *t)
{
    t->head = t->count = 0;
    t->pending = 0;
    t->packets[0] = t->packets[1] = 0;
    t->bytes[0] = t->bytes[1] = 0;
}
