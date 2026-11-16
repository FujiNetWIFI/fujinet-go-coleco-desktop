/*
 * memtools -- see core/include/colecomemtools.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "colecomemtools.h"
#include "debugger_internal.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- range parsing ------------------------------------------------------------ */

static int is_hex_char(char chr)
{
    return (chr >= '0' && chr <= '9') || (chr >= 'a' && chr <= 'f') || (chr >= 'A' && chr <= 'F');
}

static int eval_addr(colecodebug *dbg, const char *text, size_t len, long *out)
{
    char buf[128], err[64];

    while (len && isspace((unsigned char)text[0])) { text++; len--; }
    while (len && isspace((unsigned char)text[len - 1])) len--;
    if (!len || len >= sizeof buf) return 0;
    memcpy(buf, text, len);
    buf[len] = '\0';
    /* A bare 1-4 digit token is hex here (memory fields have always taken
     * "C000"), unless it names a label. */
    if (len <= 4) {
        size_t idx;
        for (idx = 0; idx < len && is_hex_char(buf[idx]); idx++) {}
        if (idx == len && colecodebug_label_address(dbg, buf) < 0) {
            *out = strtol(buf, NULL, 16);
            return 1;
        }
    }
    if (colecodebug_eval(dbg, buf, out, err, sizeof err) != 0)
        return 0;
    *out &= 0xFFFF;
    return 1;
}

/* No operator characters: a number or a label. */
static int plain(const char *text, size_t len)
{
    size_t idx;
    for (idx = 0; idx < len; idx++)
        if (strchr("+-*/%&|^<>=!~()[]{}", text[idx])) return 0;
    return 1;
}

int colecosym_parse_range(colecodebug *dbg, const char *text, uint16_t *low, uint16_t *high)
{
    const char *dots, *ptr;
    size_t len;
    long first, last;

    if (!dbg || !text) return 0;
    while (isspace((unsigned char)*text)) text++;
    len = strlen(text);
    if (!len) return 0;

    dots = strstr(text, "..");
    if (dots) {
        if (!eval_addr(dbg, text, (size_t)(dots - text), &first) ||
            !eval_addr(dbg, dots + 2, strlen(dots + 2), &last))
            return 0;
    } else {
        int split = 0;
        for (ptr = text + 1; *ptr; ptr++) {
            if (*ptr != '-') continue;
            if (plain(text, (size_t)(ptr - text)) && plain(ptr + 1, strlen(ptr + 1)) &&
                eval_addr(dbg, text, (size_t)(ptr - text), &first) &&
                eval_addr(dbg, ptr + 1, strlen(ptr + 1), &last)) {
                split = 1;
                break;
            }
        }
        if (!split) {
            if (!eval_addr(dbg, text, len, &first)) return 0;
            last = first;
        }
    }
    if (first > last) { long swap = first; first = last; last = swap; }
    *low = (uint16_t)first;
    *high = (uint16_t)last;
    return 1;
}

/* ---- watches ------------------------------------------------------------------ */

static struct {
    pthread_mutex_t mtx;
    char text[COLECOWATCH_MAX][96];
    int count;
    unsigned generation;
} g_watch = { .mtx = PTHREAD_MUTEX_INITIALIZER };

int colecowatch_count(void)
{
    int count;
    pthread_mutex_lock(&g_watch.mtx);
    count = g_watch.count;
    pthread_mutex_unlock(&g_watch.mtx);
    return count;
}

int colecowatch_add(const char *text)
{
    int slot = -1;

    if (!text) return -1;
    while (isspace((unsigned char)*text)) text++;
    if (!*text) return -1;
    pthread_mutex_lock(&g_watch.mtx);
    if (g_watch.count < COLECOWATCH_MAX) {
        size_t len;
        slot = g_watch.count++;
        snprintf(g_watch.text[slot], sizeof g_watch.text[slot], "%s", text);
        len = strlen(g_watch.text[slot]);
        while (len && isspace((unsigned char)g_watch.text[slot][len - 1]))
            g_watch.text[slot][--len] = '\0';
        g_watch.generation++;
    }
    pthread_mutex_unlock(&g_watch.mtx);
    return slot;
}

void colecowatch_remove(int index)
{
    pthread_mutex_lock(&g_watch.mtx);
    if (index >= 0 && index < g_watch.count) {
        memmove(g_watch.text[index], g_watch.text[index + 1],
                (size_t)(g_watch.count - index - 1) * sizeof g_watch.text[0]);
        g_watch.count--;
        g_watch.generation++;
    }
    pthread_mutex_unlock(&g_watch.mtx);
}

void colecowatch_clear(void)
{
    pthread_mutex_lock(&g_watch.mtx);
    g_watch.count = 0;
    g_watch.generation++;
    pthread_mutex_unlock(&g_watch.mtx);
}

/* Valid until the next change; the windows are single-threaded users. */
const char *colecowatch_text(int index)
{
    const char *text = NULL;
    pthread_mutex_lock(&g_watch.mtx);
    if (index >= 0 && index < g_watch.count) text = g_watch.text[index];
    pthread_mutex_unlock(&g_watch.mtx);
    return text;
}

unsigned colecowatch_generation(void)
{
    unsigned gen;
    pthread_mutex_lock(&g_watch.mtx);
    gen = g_watch.generation;
    pthread_mutex_unlock(&g_watch.mtx);
    return gen;
}

int colecowatch_range(colecodebug *dbg, int index, uint16_t *low, uint16_t *high)
{
    char text[96];
    const char *src = colecowatch_text(index);

    if (!src) return 0;
    snprintf(text, sizeof text, "%s", src);
    return colecosym_parse_range(dbg, text, low, high);
}

int colecowatch_format(colecodebug *dbg, int index, char *dst, int dstsz)
{
    char text[96];
    uint16_t low, high;
    const char *src = colecowatch_text(index);
    int len, idx;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!src) return 0;
    snprintf(text, sizeof text, "%s", src);
    if (!colecosym_parse_range(dbg, text, &low, &high))
        return snprintf(dst, (size_t)dstsz, "%-14s ?", text);
    len = snprintf(dst, (size_t)dstsz, "%-14s $%04X", text, low);
    if (len >= dstsz) return dstsz - 1;
    if (!colecodebug_is_paused(dbg))
        return len + snprintf(dst + len, (size_t)(dstsz - len), "  -");
    if (low == high) {
        uint8_t byte = 0;
        colecodebug_read_mem(dbg, low, &byte, 1);
        return len + snprintf(dst + len, (size_t)(dstsz - len), "  $%02X (%u)", byte, byte);
    }
    len += snprintf(dst + len, (size_t)(dstsz - len), "-$%04X ", high);
    {
        uint8_t bytes[8] = { 0 };
        const int want = high - low + 1 < 8 ? high - low + 1 : 8;
        colecodebug_read_mem(dbg, low, bytes, want);
        for (idx = 0; idx < want && len < dstsz - 4; idx++)
            len += snprintf(dst + len, (size_t)(dstsz - len), " %02X", bytes[idx]);
    }
    if (high - low >= 8 && len < dstsz - 4)
        len += snprintf(dst + len, (size_t)(dstsz - len), " ...");
    return len < dstsz ? len : dstsz - 1;
}

/* ---- search ------------------------------------------------------------------- */

static int hex_value(char chr)
{
    if (chr >= '0' && chr <= '9') return chr - '0';
    if (chr >= 'a' && chr <= 'f') return chr - 'a' + 10;
    if (chr >= 'A' && chr <= 'F') return chr - 'A' + 10;
    return -1;
}

int colecosearch_parse(int kind, const char *text, uint8_t *pat, int *count, uint8_t *mask, int *flags)
{
    int found = 0;

    if (!text || !pat || !count || !mask || !flags) return 0;
    *mask = 0xFF;
    *flags = 0;
    if (kind == COLECOSEARCH_TEXT) {
        const char *ptr;
        for (ptr = text; *ptr && found < COLECOSEARCH_MAX; ptr++)
            pat[found++] = (uint8_t)*ptr;
        *flags = COLECODEBUG_SEARCH_NOCASE;
    } else {
        const char *ptr = text;
        while (*ptr) {
            int high, low;
            if (isspace((unsigned char)*ptr) || *ptr == ',') { ptr++; continue; }
            if (*ptr == '$') { ptr++; continue; }
            if (ptr[0] == '0' && (ptr[1] == 'x' || ptr[1] == 'X')) { ptr += 2; continue; }
            high = hex_value(*ptr);
            if (high < 0) return 0;
            low = hex_value(ptr[1]);
            if (low < 0) { low = high; high = 0; ptr += 1; }   /* a lone digit is a byte */
            else ptr += 2;
            if (found >= COLECOSEARCH_MAX) break;
            pat[found++] = (uint8_t)(high << 4 | low);
        }
    }
    *count = found;
    return found > 0;
}

/* ---- breakpoint description ----------------------------------------------------- */

int colecobp_describe(colecodebug *dbg, const colecodebug_breakpoint *bpt, char *dst, int dstsz)
{
    char kind[48] = "", label[48] = "";
    int len;
    const int is_port = bpt && (bpt->type & (COLECODEBUG_BP_IN | COLECODEBUG_BP_OUT)) != 0;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!bpt) return 0;
    if ((bpt->type & COLECODEBUG_BP_ACCESS) == COLECODEBUG_BP_ACCESS) {
        snprintf(kind, sizeof kind, "Access");
    } else if (bpt->type & COLECODEBUG_BP_EXEC) {
        snprintf(kind, sizeof kind, "Exec");
    } else {
        static const struct { int bit; const char *name; } names[] = {
            { COLECODEBUG_BP_READ, "Read" }, { COLECODEBUG_BP_WRITE, "Write" },
            { COLECODEBUG_BP_IN, "In" }, { COLECODEBUG_BP_OUT, "Out" },
        };
        size_t idx;
        for (idx = 0; idx < sizeof names / sizeof names[0]; idx++)
            if (bpt->type & names[idx].bit) {
                if (kind[0]) strncat(kind, "/", sizeof kind - strlen(kind) - 1);
                strncat(kind, names[idx].name, sizeof kind - strlen(kind) - 1);
            }
    }
    /* a label only names a CPU address */
    if (!is_port) colecodebug_address_label(dbg, bpt->start, label, sizeof label);
    if (bpt->start == bpt->end)
        len = snprintf(dst, (size_t)dstsz, is_port ? "%s  $%02X" : "%s  $%04X", kind, bpt->start);
    else
        len = snprintf(dst, (size_t)dstsz, is_port ? "%s  $%02X-$%02X" : "%s  $%04X-$%04X",
                       kind, bpt->start, bpt->end);
    if (len < dstsz && label[0])
        len += snprintf(dst + len, (size_t)(dstsz - len), " (%s)", label);
    if (len < dstsz && bpt->condition[0])
        len += snprintf(dst + len, (size_t)(dstsz - len), "  if %s", bpt->condition);
    return len < dstsz ? len : dstsz - 1;
}

/* ---- memory map labels ---------------------------------------------------------- */

int colecomem_region_name(const colecodebug_region *map, int count, uint16_t addr, char *dst, int dstsz)
{
    int idx;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    for (idx = 0; map && idx < count; idx++)
        if (addr >= map[idx].low && addr <= map[idx].high)
            return snprintf(dst, (size_t)dstsz, "%s", map[idx].name);
    return 0;
}
