/*
 * roms_import_test -- Import BIOS, end to end and without a machine: the
 * BIOS is recognised by size and CRC-32 whatever the file is called and stored
 * as OS7.rom; a file, a zip (stored) and a folder (8 levels deep, zips inside
 * it, zips inside zips) are searched; an unknown 8K file is taken only when it
 * is the one file the user handed over; a dropped BIOS is a ROM, not a
 * cartridge.
 *
 * No firmware is used: the images are filler whose last four bytes steer the
 * CRC-32 to the wanted value (CRC-32 is linear), written by this test, and the
 * zips come from the test's own tiny stored-zip writer.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define test_mkdir(p) _mkdir(p)
#define getpid _getpid
#else
#include <unistd.h>
#define test_mkdir(p) mkdir(p, 0755)
#endif

#include "colecosession.h"
#include "zipread.h"

#define BIOS_SIZE 8192u
#define BIOS_CRC 0x3AA93EF3u
#define BIOS_CRC_ALT 0x39BB16FCu

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

/* ---- CRC-32, and filler with a chosen one --------------------------------- */

static uint32_t T[256];
static uint8_t inv[256];        /* T's top bytes are a permutation */

static void crc_tables(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (~(c & 1) + 1));
        T[i] = c;
        inv[c >> 24] = (uint8_t)i;
    }
}

static uint32_t crc_of(const uint8_t *p, size_t n)
{
    uint32_t r = 0xffffffffu;
    while (n--)
        r = (r >> 8) ^ T[(r ^ *p++) & 0xff];
    return ~r;
}

/* `size` bytes of seeded filler ending in the four bytes that make the CRC-32
 * `want`: pick the four table entries whose sum is the final register, then
 * the bytes that select them from the register the filler leaves. */
static void forge(uint8_t *buf, uint32_t size, uint32_t seed, uint32_t want)
{
    uint32_t r = 0xffffffffu, v = ~want;
    uint8_t idx[4];

    for (uint32_t i = 0; i < size - 4; i++) {
        seed = seed * 1103515245u + 12345u;
        buf[i] = (uint8_t)(seed >> 16);
        r = (r >> 8) ^ T[(r ^ buf[i]) & 0xff];
    }
    idx[3] = inv[v >> 24];          v ^= T[idx[3]];
    idx[2] = inv[(v >> 16) & 0xff]; v ^= T[idx[2]] >> 8;
    idx[1] = inv[(v >> 8) & 0xff];  v ^= T[idx[1]] >> 16;
    idx[0] = inv[v & 0xff];
    for (int k = 0; k < 4; k++) {
        buf[size - 4 + k] = (uint8_t)(idx[k] ^ (r & 0xff));
        r = (r >> 8) ^ T[idx[k]];
    }
}

/* ---- files, folders, zips ---------------------------------------------------- */

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static long size_of(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static void write_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL: cannot write %s\n", path); failures++; return; }
    fwrite(data, 1, n, f);
    fclose(f);
}

static void mkpath(const char *dir)
{
    char buf[1024];
    char *p;
    snprintf(buf, sizeof buf, "%s", dir);
    for (p = buf + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            test_mkdir(buf);
            *p = '/';
        }
    test_mkdir(buf);
}

/* A BIOS-sized image with the given CRC. Caller frees. */
static uint8_t *image(uint32_t seed, uint32_t crc)
{
    uint8_t *b = malloc(BIOS_SIZE);
    forge(b, BIOS_SIZE, seed, crc);
    return b;
}

static void write_image(const char *path, uint32_t seed, uint32_t crc)
{
    uint8_t *b = image(seed, crc);
    write_file(path, b, BIOS_SIZE);
    free(b);
}

static uint8_t *read_all(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    if (!f) { *n = -1; return NULL; }
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)*n + 1);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) *n = -1;
    fclose(f);
    return b;
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

typedef struct {
    const char *name;
    const uint8_t *data;       /* what the entry holds (stored) or the deflate stream */
    uint32_t csize;            /* bytes in the file */
    uint32_t usize, crc;
    int method;                /* 0 stored, 8 deflated */
} zent;

/* A zip, the way `zip` writes one: local headers, the central directory, the
 * end record. */
static void write_zip(const char *path, const zent *e, int n)
{
    FILE *f = fopen(path, "wb");
    uint32_t off[16], cd_off, cd_size = 0;
    uint8_t h[46];
    int i;

    if (!f) { printf("FAIL: cannot write %s\n", path); failures++; return; }
    for (i = 0; i < n; i++) {
        size_t nl = strlen(e[i].name);
        off[i] = (uint32_t)ftell(f);
        memset(h, 0, sizeof h);
        put32(h, 0x04034b50u); put16(h + 4, 20); put16(h + 8, (unsigned)e[i].method);
        put16(h + 12, 0x21); put32(h + 14, e[i].crc);
        put32(h + 18, e[i].csize); put32(h + 22, e[i].usize); put16(h + 26, (unsigned)nl);
        fwrite(h, 1, 30, f);
        fwrite(e[i].name, 1, nl, f);
        fwrite(e[i].data, 1, e[i].csize, f);
    }
    cd_off = (uint32_t)ftell(f);
    for (i = 0; i < n; i++) {
        size_t nl = strlen(e[i].name);
        memset(h, 0, sizeof h);
        put32(h, 0x02014b50u); put16(h + 4, 20); put16(h + 6, 20); put16(h + 10, (unsigned)e[i].method);
        put16(h + 14, 0x21); put32(h + 16, e[i].crc);
        put32(h + 20, e[i].csize); put32(h + 24, e[i].usize); put16(h + 28, (unsigned)nl);
        put32(h + 42, off[i]);
        fwrite(h, 1, 46, f);
        fwrite(e[i].name, 1, nl, f);
        cd_size += 46 + (uint32_t)nl;
    }
    memset(h, 0, 22);
    put32(h, 0x06054b50u); put16(h + 8, (unsigned)n); put16(h + 10, (unsigned)n);
    put32(h + 12, cd_size); put32(h + 16, cd_off);
    fwrite(h, 1, 22, f);
    fclose(f);
}

static zent stored(const char *name, const uint8_t *data, uint32_t n)
{
    zent z;
    z.name = name; z.data = data; z.csize = z.usize = n;
    z.crc = crc_of(data, n); z.method = 0;
    return z;
}

static colecosession *new_session(const char *tag, char *data, size_t datasz,
                                  char *work, size_t worksz)
{
    colecosession_paths paths;
    char cfg[700], root[600];

    snprintf(root, sizeof root, "/tmp/coleco-roms-%s-%ld-%ld", tag, (long)getpid(), (long)time(NULL));
    snprintf(cfg, sizeof cfg, "%s/config", root);
    snprintf(data, datasz, "%s/data", root);
    snprintf(work, worksz, "%s/work", root);
    mkpath(root);
    mkpath(work);
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";
    return colecosession_new(&paths);
}

int main(void)
{
    colecosession *s;
    char data[700], work[700], path[1024], dest[1024], msg[1024];
    uint8_t *buf, *img;
    long n;
    int rc, i;

    crc_tables();
    check(zipread_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926u, "the reader's CRC-32 is the IEEE one");

    s = new_session("a", data, sizeof data, work, sizeof work);
    if (!s) { printf("FAIL: no session\n"); return 1; }
    snprintf(dest, sizeof dest, "%s/OS7.rom", colecosession_roms_path(s));
    check(!colecosession_bios_available(s), "nothing is imported yet");

    /* ---- one file, any name ---- */
    snprintf(path, sizeof path, "%s/ColecoVision BIOS (1982).bin", work);
    write_image(path, 1, BIOS_CRC);
    check(colecosession_media_is_rom(path), "a BIOS dump is a ROM by size and CRC, though it is a .bin");
    check(!colecosession_media_is_cartridge(path), "and not a cartridge");
    rc = colecosession_rom_import(s, path, msg, sizeof msg);
    check(rc == 1, "importing it takes one item");
    check(strstr(msg, "OS7.rom") != NULL && strstr(msg, "next power cycle") != NULL, "and says what and when");
    check(size_of(dest) == (long)BIOS_SIZE, "it is stored as OS7.rom");
    buf = read_all(dest, &n);
    check(n == (long)BIOS_SIZE && crc_of(buf, BIOS_SIZE) == BIOS_CRC, "byte for byte");
    free(buf);
    snprintf(path, sizeof path, "%s.part", dest);
    check(size_of(path) < 0, "with no .part file left behind");
    check(colecosession_bios_available(s), "the BIOS is available");

    /* the other known revision, under yet another name */
    snprintf(path, sizeof path, "%s/telegames.col", work);
    write_image(path, 2, BIOS_CRC_ALT);
    check(colecosession_media_is_rom(path), "the Dina / Telegames BIOS is a ROM too");
    check(colecosession_rom_import(s, path, msg, sizeof msg) == 1, "and imports");
    buf = read_all(dest, &n);
    check(n == (long)BIOS_SIZE && crc_of(buf, BIOS_SIZE) == BIOS_CRC_ALT, "replacing the first");
    free(buf);

    /* ---- not a BIOS ---- */
    snprintf(path, sizeof path, "%s/cart.rom", work);
    write_image(path, 3, 0x12345678u);
    check(!colecosession_media_is_rom(path), "an 8K file of an unknown CRC is a cartridge to a drop");
    snprintf(path, sizeof path, "%s/small.bin", work);
    {
        uint8_t small[100];
        memset(small, 7, sizeof small);
        write_file(path, small, sizeof small);
        rc = colecosession_rom_import(s, path, msg, sizeof msg);
        check(rc == 0, "a file of the wrong size imports nothing");
        check(strstr(msg, "small.bin") != NULL && strstr(msg, "100 bytes") != NULL
              && strstr(msg, "8,192") != NULL, "and says what the BIOS is");
    }
    rc = colecosession_rom_import(s, "/nonexistent/os7.rom", msg, sizeof msg);
    check(rc < 0 && msg[0] && colecosession_last_error(s)[0], "a missing file is an error with a reason");

    /* ---- unknown CRC: only the one file handed over ---- */
    colecosession_free(s);
    s = new_session("b", data, sizeof data, work, sizeof work);
    snprintf(dest, sizeof dest, "%s/OS7.rom", colecosession_roms_path(s));
    snprintf(path, sizeof path, "%s/homebrew.rom", work);
    write_image(path, 4, 0xDEADBEEFu);
    rc = colecosession_rom_import(s, path, msg, sizeof msg);
    check(rc == 1 && size_of(dest) == (long)BIOS_SIZE, "an unknown 8K file handed over alone is taken");
    check(strstr(msg, "DEADBEEF") != NULL && strstr(msg, "misbehaves") != NULL, "with its CRC in a warning");
    check(colecosession_import_bios(s, path) == 1 && colecosession_last_error(s)[0],
          "colecosession_import_bios still returns 1 and the warning");
    remove(dest);
    {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/sweep", work);
        mkpath(dir);
        snprintf(path, sizeof path, "%s/unknown.rom", dir);
        write_image(path, 5, 0xDEADBEEFu);
        rc = colecosession_rom_import(s, dir, msg, sizeof msg);
        check(rc == 0 && size_of(dest) < 0, "but a folder sweep never guesses");
        check(strstr(msg, "No ColecoVision BIOS was found") != NULL, "and says nothing was found");
    }

    /* ---- zip ---- */
    snprintf(path, sizeof path, "%s/coleco.zip", work);
    img = image(6, BIOS_CRC);
    {
        const uint8_t junk[16] = { 1, 2, 3 };
        zent e[2];
        e[0] = stored("readme.txt", junk, sizeof junk);
        e[1] = stored("313 10031-4005 73108a.u2", img, BIOS_SIZE);
        write_zip(path, e, 2);
    }
    check(colecosession_media_is_rom(path), "a zip holding the BIOS is a ROM");
    rc = colecosession_rom_import(s, path, msg, sizeof msg);
    check(rc == 1 && size_of(dest) == (long)BIOS_SIZE, "importing it finds the BIOS inside, whatever its name");
    free(img);
    remove(dest);

    /* an unknown-CRC image inside a zip is never guessed, even as the one file */
    snprintf(path, sizeof path, "%s/unknown.zip", work);
    img = image(7, 0xDEADBEEFu);
    {
        zent e[1];
        e[0] = stored("x.rom", img, BIOS_SIZE);
        write_zip(path, e, 1);
    }
    check(!colecosession_media_is_rom(path), "a zip of an unknown 8K image is not a ROM");
    rc = colecosession_rom_import(s, path, msg, sizeof msg);
    check(rc == 0 && size_of(dest) < 0, "and imports nothing");
    free(img);

    /* ---- folder, deep, with a zip inside, and a zip in a zip ---- */
    {
        char deep[1024], inner[1024], outer[1024];
        uint8_t *zipbytes;
        long zn;
        zent e[1];

        snprintf(deep, sizeof deep, "%s/roms/a/b/c/d/e/f/g/h", work);
        mkpath(deep);
        snprintf(path, sizeof path, "%s/roms/a/b/c/d/e/f/g/h/coleco.bin", work);
        write_image(path, 8, BIOS_CRC);
        snprintf(path, sizeof path, "%s/roms", work);
        rc = colecosession_rom_import(s, path, msg, sizeof msg);
        check(rc == 1 && size_of(dest) == (long)BIOS_SIZE, "a folder is searched 8 levels deep");
        remove(dest);

        snprintf(deep, sizeof deep, "%s/toodeep/a/b/c/d/e/f/g/h/i", work);
        mkpath(deep);
        snprintf(path, sizeof path, "%s/toodeep/a/b/c/d/e/f/g/h/i/coleco.bin", work);
        write_image(path, 9, BIOS_CRC);
        snprintf(path, sizeof path, "%s/toodeep", work);
        rc = colecosession_rom_import(s, path, msg, sizeof msg);
        check(rc == 0, "and no deeper");

        snprintf(inner, sizeof inner, "%s/zipdir/inner.zip", work);
        snprintf(path, sizeof path, "%s/zipdir", work);
        mkpath(path);
        img = image(10, BIOS_CRC);
        e[0] = stored("os7.rom", img, BIOS_SIZE);
        write_zip(inner, e, 1);
        free(img);
        rc = colecosession_rom_import(s, path, msg, sizeof msg);
        check(rc == 1 && size_of(dest) == (long)BIOS_SIZE, "a zip found in a folder is opened");
        remove(dest);

        zipbytes = read_all(inner, &zn);
        snprintf(outer, sizeof outer, "%s/outer.zip", work);
        e[0] = stored("inner.zip", zipbytes, (uint32_t)zn);
        write_zip(outer, e, 1);
        free(zipbytes);
        rc = colecosession_rom_import(s, outer, msg, sizeof msg);
        check(rc == 1 && size_of(dest) == (long)BIOS_SIZE, "and a zip inside a zip");
        remove(dest);
    }

    /* ---- a dropped BIOS goes to the ROM directory, not the cartridges ---- */
    snprintf(path, sizeof path, "%s/dropped.rom", work);
    write_image(path, 11, BIOS_CRC);
    {
        char out[1024];
        rc = colecosession_import_media(s, path, out, sizeof out);
        check(rc == 0 && strstr(out, colecosession_roms_path(s)) == out, "a dropped BIOS is routed to the ROM directory");
        check(size_of(dest) == (long)BIOS_SIZE, "and is there as OS7.rom");
        snprintf(path, sizeof path, "%s/dropped.rom", colecosession_carts_path(s));
        check(size_of(path) < 0, "and not copied into the cartridges");
    }
    colecosession_free(s);
    (void)i;

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
