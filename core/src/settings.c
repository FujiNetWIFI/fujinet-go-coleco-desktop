/*
 * colecosession settings store: one flat key=value file shared by every
 * frontend, so a preference chosen in one app is what the next one launches
 * with. Deliberately not GSettings/KConfig. Transposed from the Intv port's
 * settings.c (same author, same license).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session_internal.h"

static setting_kv *find(struct colecosession *s, const char *key)
{
    setting_kv *kv;
    for (kv = s->settings; kv; kv = kv->next)
        if (strcmp(kv->key, key) == 0)
            return kv;
    return NULL;
}

static void set_locked(struct colecosession *s, const char *key, const char *val)
{
    setting_kv *kv = find(s, key);
    if (kv) {
        if (strcmp(kv->val, val) != 0) {
            free(kv->val);
            kv->val = strdup(val);
            s->settings_dirty = 1;
        }
        return;
    }
    kv = calloc(1, sizeof(*kv));
    if (!kv) return;
    kv->key = strdup(key);
    kv->val = strdup(val);
    kv->next = s->settings;
    s->settings = kv;
    s->settings_dirty = 1;
}

void settings_init(struct colecosession *sess)
{
    FILE *file;
    char line[16384];   /* pad_assign alone can run to several KB */

    pthread_mutex_init(&sess->settings_mtx, NULL);
    file = fopen(sess->settings_file, "r");
    if (!file) return;
    while (fgets(line, sizeof(line), file)) {
        char *equals, *end;
        char *cursor = line;
        if (!strchr(line, '\n') && !feof(file)) {
            /* longer than any value we write: drop the line whole rather
             * than parse its tail as another setting */
            int chr;
            while ((chr = fgetc(file)) != EOF && chr != '\n') {}
            continue;
        }
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        if (*cursor == '#' || *cursor == '\n' || *cursor == '\0') continue;
        equals = strchr(cursor, '=');
        if (!equals) continue;
        *equals = '\0';
        end = equals + 1 + strcspn(equals + 1, "\r\n");
        *end = '\0';
        set_locked(sess, cursor, equals + 1);
    }
    fclose(file);
    sess->settings_dirty = 0;
}

void settings_free_all(struct colecosession *s)
{
    setting_kv *kv = s->settings;
    while (kv) {
        setting_kv *next = kv->next;
        free(kv->key);
        free(kv->val);
        free(kv);
        kv = next;
    }
    s->settings = NULL;
    pthread_mutex_destroy(&s->settings_mtx);
}

const char *colecosession_get_str(colecosession *s, const char *key,
                                 const char *def)
{
    setting_kv *kv;
    const char *v;
    pthread_mutex_lock(&s->settings_mtx);
    kv = find(s, key);
    v = kv ? kv->val : def;
    pthread_mutex_unlock(&s->settings_mtx);
    return v;
}

void colecosession_set_str(colecosession *s, const char *key, const char *value)
{
    pthread_mutex_lock(&s->settings_mtx);
    set_locked(s, key, value ? value : "");
    pthread_mutex_unlock(&s->settings_mtx);
    colecosession_settings_flush(s);
}

int colecosession_get_int(colecosession *s, const char *key, int def)
{
    const char *v = colecosession_get_str(s, key, NULL);
    if (!v || !*v) return def;
    return atoi(v);
}

void colecosession_set_int(colecosession *s, const char *key, int value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    colecosession_set_str(s, key, buf);
}

void colecosession_settings_flush(colecosession *s)
{
    char tmp[COLECO_PATH_MAX + 8];
    FILE *fp;
    setting_kv *kv;

    pthread_mutex_lock(&s->settings_mtx);
    if (!s->settings_dirty) {
        pthread_mutex_unlock(&s->settings_mtx);
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", s->settings_file);
    fp = fopen(tmp, "w");
    if (fp) {
        fprintf(fp, "# FujiNet Go ColecoVision shared settings\n");
        for (kv = s->settings; kv; kv = kv->next)
            fprintf(fp, "%s=%s\n", kv->key, kv->val);
        fclose(fp);
#if defined(_WIN32)
        remove(s->settings_file);
#endif
        rename(tmp, s->settings_file);
        s->settings_dirty = 0;
    }
    pthread_mutex_unlock(&s->settings_mtx);
}
