/*
 * colecohelp -- the app's Help: data/help/help.md, built in by
 * tools/help/make-help.py as topics of blocks of styled spans, which every
 * frontend lays out with its own toolkit.
 *
 * A frontend shows the blocks whose platforms include its own bit. Ids are
 * GitHub's heading slugs ("controllers-and-keys"); a Help button names a
 * context instead ("prefs/Machine", "debugger/cpu"), which stays put when a
 * heading is renamed. Everything here is constant data and reentrant.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef COLECOHELP_H
#define COLECOHELP_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    COLECOHELP_GNOME = 1, COLECOHELP_KDE = 2, COLECOHELP_WINDOWS = 4, COLECOHELP_MACOS = 8
};

typedef enum {
    COLECOHELP_H2 = 0, COLECOHELP_H3,
    COLECOHELP_PARA,
    COLECOHELP_BULLET, COLECOHELP_BULLET2,    /* "- " and "  - " */
    COLECOHELP_NUMBERED,                     /* a step; number is its own */
    COLECOHELP_NOTE,
    COLECOHELP_PRE,                          /* one TEXT span, lines as they are */
    COLECOHELP_TABLE_HEAD,                   /* starts a table; its rows follow */
    COLECOHELP_TABLE_ROW
} colecohelp_block_kind;

typedef enum {
    COLECOHELP_TEXT = 0, COLECOHELP_BOLD, COLECOHELP_ITALIC, COLECOHELP_CODE,
    COLECOHELP_KBD,                          /* keys; mac_text is the Mac's */
    COLECOHELP_LINK,                         /* target "#slug" or a URL */
    COLECOHELP_CELL                          /* tables: the next column starts */
} colecohelp_span_kind;

typedef struct {
    int kind;
    const char *text;                       /* UTF-8 */
    const char *mac_text;                   /* KBD on macOS, or NULL: text */
    const char *target;
} colecohelp_span;

typedef struct {
    int kind;
    unsigned platforms;                     /* COLECOHELP_* bits */
    int number;
    const char *id;                         /* headings: the slug */
    const char *context;                    /* headings: or NULL */
    const colecohelp_span *spans;            /* headings: one TEXT span */
    int nspans;
    /* Tables: the width of each column but the last, in ems, for frontends
     * that set tab stops (the last column wraps under itself). */
    const unsigned char *col_em;
    int ncol_em;
} colecohelp_block;

typedef struct {
    const char *id;
    const char *title;
    const char *context;                    /* or NULL */
    const colecohelp_block *blocks;
    int nblocks;
} colecohelp_topic;

int colecohelp_topic_count(void);
const colecohelp_topic *colecohelp_topic_at(int t);

/* "slug", "#slug" or a context: its topic, and the heading's block (-1 for
 * the topic itself). 0, or -1 if there is none. */
int colecohelp_find(const char *id, int *topic, int *block);

/* The span's text as platform shows it. */
const char *colecohelp_span_text(const colecohelp_span *sp, unsigned platform);

/* A heading block's text. */
const char *colecohelp_heading(const colecohelp_block *b);

/* The block after the section headed by block (the next heading at its
 * level or above, or the topic's end); -1: the whole topic. */
int colecohelp_section_end(int topic, int block);

/* The section headed by block (-1: from the topic's title) as text on
 * platform: a line per block, cells separated by " | ". Returns its length
 * (truncated to dstsz - 1). */
int colecohelp_plain_text(unsigned platform, int topic, int block, char *dst, int dstsz);

/* Sections that have every word of query (case-insensitive), best first:
 * where it is in a title, then how often. block -1 is a topic's start, up
 * to its first heading. Returns how many were written. */
typedef struct {
    int topic, block, score;
} colecohelp_hit;
int colecohelp_search(unsigned platform, const char *query, colecohelp_hit *out, int max);

/* The first match of a word of query in text at byte from or after: where
 * it starts and its length, for highlighting. 1, or 0 if there is none. */
int colecohelp_match(const char *text, const char *query, int from, int *start, int *len);

#ifdef __cplusplus
}
#endif

#endif /* COLECOHELP_H */
