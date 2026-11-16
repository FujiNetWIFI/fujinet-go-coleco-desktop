/*
 * iostate.c -- the machine as the Cart & I/O and Sound tabs show it: the
 * controllers, the VDP's host side, the CPU's interrupt state, the Super Game
 * Module, the cartridge's mapper, and the two sound chips. Everything comes
 * from adamcore's emulator-thread accessors (adamcore_debug.h) -- never from
 * the live sound chips, which the audio thread owns -- and is read under the
 * engine lock, so a session restart cannot free the core underneath.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../coleco/fujinet_cart.h"
#include "../coleco/host.h"
#include "debugger_internal.h"

typedef struct {
    char *dst;
    int cap, len;
} outbuf;

static void emit(outbuf *buf, const char *fmt, ...)
{
    va_list args;
    int wrote;
    if (buf->len >= buf->cap - 1) return;
    va_start(args, fmt);
    wrote = vsnprintf(buf->dst + buf->len, (size_t)(buf->cap - buf->len), fmt, args);
    va_end(args);
    if (wrote > 0)
        buf->len += wrote < buf->cap - buf->len ? wrote : buf->cap - buf->len - 1;
}

/* ---- the controllers ------------------------------------------------------- */

/* The keypad wire codes adamcore's controller_read returns, by key: the
 * inverse of its table, kept here because the table is private to it. */
static const uint8_t k_keypad_code[12] = {
    0x0A, 0x0D, 0x07, 0x0C, 0x02, 0x03, 0x0E, 0x05, 0x01, 0x0B, 0x09, 0x06
};

static int keypad_key(uint8_t low_byte)
{
    const uint8_t nibble = (uint8_t)(low_byte & 0x0F);
    int key;
    /* The word holds the key INDEX (0-9, 10 '*', 11 '#', 12+ none), exactly
     * what controller_read feeds its table with. */
    key = nibble;
    return key < 12 ? key : -1;
}

static void keypad_name(int key, char *dst)
{
    if (key < 0)
        dst[0] = '\0';
    else if (key < 10) {
        dst[0] = (char)('0' + key);
        dst[1] = '\0';
    } else {
        dst[0] = key == 10 ? '*' : '#';
        dst[1] = '\0';
    }
}

/* ---- Cart & I/O -------------------------------------------------------------- */

int colecodebug_io_get(colecodebug *dbg, colecodebug_io *out)
{
    adamcore *core;
    adamcore_vdp_latch latch;
    adamcore_cpu_aux_state aux;
    adamcore_sgm_info sgm;
    uint16_t joy[2];
    uint8_t joy_mode = 0;
    int port, line = 0;
    uint64_t frame = 0;

    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!dbg) return 0;
    colecodebug_lock(dbg);
    core = colecodebug_core_locked(dbg);
    if (!core) {
        colecodebug_unlock(dbg);
        return 0;
    }
    out->valid = 1;

    adamcore_controller_state(core, joy, &joy_mode);
    out->joy_mode = joy_mode;
    for (port = 0; port < 2; port++) {
        const uint8_t high = (uint8_t)(joy[port] >> 8);
        const uint8_t low_byte = (uint8_t)(joy[port] & 0xFF);
        const int key = keypad_key(low_byte);
        out->pad[port].raw = joy[port];
        out->pad[port].joystick_byte = (uint8_t)(0xB0 | (high & 0x4F));
        out->pad[port].keypad_byte =
            (uint8_t)(0xB0 | (low_byte & 0x40) | (key < 0 ? 0x0F : k_keypad_code[key]));
        out->pad[port].up = !(high & 0x01);
        out->pad[port].right = !(high & 0x02);
        out->pad[port].down = !(high & 0x04);
        out->pad[port].left = !(high & 0x08);
        out->pad[port].fire_left = !(high & 0x40);
        out->pad[port].fire_right = !(low_byte & 0x40);
        out->pad[port].keypad = key;
        keypad_name(key, out->pad[port].keypad_name);
    }

    adamcore_vdp_state(core, out->vdp_regs, &out->vdp_status, &out->vdp_addr);
    adamcore_vdp_latch_state(core, &latch);
    out->vdp_latch = latch.latch;
    out->vdp_latched = latch.latched;
    out->vdp_read_ahead = latch.read_ahead;
    out->vdp_int_line = latch.int_line;

    adamcore_cpu_aux(core, &aux);
    out->cpu_q = aux.q;
    out->ei_pending = aux.ei_pending;
    out->irq_pending = aux.irq_pending;
    out->nmi_pending = aux.nmi_pending;
    out->nmi_count = aux.nmi_count;

    adamcore_debug_beam(core, &line, &frame);
    out->beam_line = line;
    out->beam_tstate = adamcore_debug_beam_tstate(core);
    out->frames = frame;

    {
        int game = 0;
        adamcore_get_machine_state(core, &out->mem_ctrl, &out->net_ctrl, &game);
        out->game_mode = game;
    }

    adamcore_sgm_state(core, &sgm);
    out->sgm = sgm.configured;
    out->sgm_ram_en = sgm.ram_en;
    out->sgm_bios_off = sgm.bios_off;
    out->ay_addr = sgm.ay_addr;
    memcpy(out->ay_regs, sgm.ay_regs, sizeof(out->ay_regs));
    colecodebug_unlock(dbg);
    return 1;
}

static const char *pad_text(const colecodebug_io *state, int port, char *dst, size_t cap)
{
    size_t len = 0;
    dst[0] = '\0';
#define ADD(flag, word) do { if (state->pad[port].flag) len += (size_t)snprintf(dst + len, cap - len, "%s%s", len ? " " : "", word); } while (0)
    ADD(up, "up");
    ADD(right, "right");
    ADD(down, "down");
    ADD(left, "left");
    ADD(fire_left, "fire-L");
    ADD(fire_right, "fire-R");
#undef ADD
    if (state->pad[port].keypad >= 0)
        len += (size_t)snprintf(dst + len, cap - len, "%skey %s", len ? " " : "",
                                state->pad[port].keypad_name);
    if (!len)
        snprintf(dst, cap, "idle");
    return dst;
}

int colecodebug_io_state(colecodebug *dbg, char *dst, int dstsz)
{
    colecodebug_io state;
    outbuf buf = { dst, dstsz, 0 };
    char pad[96];
    int port, idx;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!colecodebug_io_get(dbg, &state)) return 0;

    emit(&buf, "Controllers   strobe %s half ($%s)\n", state.joy_mode ? "joystick" : "keypad",
         state.joy_mode ? "C0" : "80");
    for (port = 0; port < 2; port++)
        emit(&buf, "  port %d      word %04X  reads %02X / %02X  %s\n", port + 1, state.pad[port].raw,
             state.pad[port].joystick_byte, state.pad[port].keypad_byte,
             pad_text(&state, port, pad, sizeof pad));
    emit(&buf, "VDP           R0-7");
    for (idx = 0; idx < 8; idx++) emit(&buf, " %02X", state.vdp_regs[idx]);
    emit(&buf, "  status %02X  address %04X\n", state.vdp_status, state.vdp_addr);
    emit(&buf, "  control port  latch %02X %s  read-ahead %02X  INT %s\n", state.vdp_latch,
         state.vdp_latched ? "(waiting for the second byte)" : "(idle)", state.vdp_read_ahead,
         state.vdp_int_line ? "asserted" : "low");
    emit(&buf, "CPU           NMI %s  IRQ %s  EI shadow %s  Q %02X  VBlank NMIs %llu\n",
         state.nmi_pending ? "pending" : "none", state.irq_pending ? "pending" : "none",
         state.ei_pending ? "yes" : "no", state.cpu_q, (unsigned long long)state.nmi_count);
    emit(&buf, "Beam          line %d, T-state %d, %llu frames\n", state.beam_line, state.beam_tstate,
         (unsigned long long)state.frames);
    emit(&buf, "Memory        control %02X  net %02X  %s mode\n", state.mem_ctrl, state.net_ctrl,
         state.game_mode ? "game" : "ADAM");
    if (!state.sgm) {
        emit(&buf, "Super Game Module   not fitted\n");
    } else {
        emit(&buf, "Super Game Module   RAM %s, BIOS %s, AY register %X\n",
             state.sgm_ram_en ? "on" : "off", state.sgm_bios_off ? "off (RAM)" : "on", state.ay_addr);
        emit(&buf, "  AY");
        for (idx = 0; idx < 16; idx++) emit(&buf, " %02X", state.ay_regs[idx]);
        emit(&buf, "\n");
    }
    return buf.len;
}

/* ---- the cartridge ------------------------------------------------------------ */

void colecodebug_cart_info(colecodebug *dbg, colecodebug_cart *out)
{
    adamcore *core;
    adamcore_sgm_info sgm;
    struct coleco_cart *cart;

    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->bank = -1;
    if (!dbg) return;
    colecodebug_lock(dbg);
    core = colecodebug_core_locked(dbg);
    if (!core) {
        colecodebug_unlock(dbg);
        return;
    }
    out->valid = 1;
    snprintf(out->machine, sizeof(out->machine), "ColecoVision");
    adamcore_sgm_state(core, &sgm);
    out->sgm = sgm.configured;
    cart = coleco_host_cart();
    if (cart) {
        coleco_cart_info info;
        coleco_cart_get_info(cart, &info);
        out->cartridge = 1;
        out->fujinet_link = info.link_up;
        out->mailbox_live = info.mailbox_live;
        snprintf(out->kind, sizeof(out->kind), "%s", colmap_kindname(info.kind));
        out->size = info.size;
        if (info.kind == COLMAP_MEGACART || info.kind == COLMAP_ACTIVISION)
            out->bank = info.bank;
        snprintf(out->hostport, sizeof(out->hostport), "%s", info.hostport);
    }
    colecodebug_unlock(dbg);
}

int colecodebug_cart_summary(colecodebug *dbg, char *dst, int dstsz)
{
    colecodebug_cart cart;
    outbuf buf = { dst, dstsz, 0 };

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    colecodebug_cart_info(dbg, &cart);
    if (!cart.valid) {
        emit(&buf, "no machine");
        return buf.len;
    }
    emit(&buf, "%s", cart.machine);
    if (cart.cartridge) {
        emit(&buf, ", FujiNet cartridge (%s %u bytes", cart.kind, cart.size);
        if (cart.bank >= 0) emit(&buf, ", bank %d", cart.bank);
        emit(&buf, ", link %s", cart.fujinet_link ? "up" : "down");
        if (cart.mailbox_live) emit(&buf, ", mailbox live");
        emit(&buf, ")");
    } else {
        emit(&buf, ", no cartridge device");
    }
    if (cart.sgm) emit(&buf, ", Super Game Module");
    return buf.len;
}

/* ---- the memory map ------------------------------------------------------------- */

typedef struct {
    colecodebug_region *out;
    int max, count;
} map_builder;

/* Adds a region, joining it to the previous one when the name and the access
 * are the same. */
static void map_add(map_builder *map, uint16_t low, uint16_t high, int readable, int writable,
                    const char *name)
{
    if (map->count > 0) {
        colecodebug_region *last = &map->out[map->count - 1];
        if (last->readable == readable && last->writable == writable &&
            !strcmp(last->name, name) && (unsigned)last->high + 1 == low) {
            last->high = high;
            return;
        }
    }
    if (map->count >= map->max) return;
    map->out[map->count].low = low;
    map->out[map->count].high = high;
    map->out[map->count].readable = readable;
    map->out[map->count].writable = writable;
    snprintf(map->out[map->count].name, sizeof(map->out[map->count].name), "%s", name);
    map->count++;
}

int colecodebug_memory_map(colecodebug *dbg, colecodebug_region *out, int max)
{
    map_builder map = { out, max, 0 };
    adamcore *core;
    adamcore_sgm_info sgm;
    uint8_t mem_ctrl = 0, net_ctrl = 0;
    int game = 0;
    char text[64];
    struct coleco_cart *cart;
    coleco_cart_info info;
    int have_cart;

    if (!out || max <= 0) return 0;
    if (!dbg) return 0;
    colecodebug_lock(dbg);
    core = colecodebug_core_locked(dbg);
    if (!core) {
        colecodebug_unlock(dbg);
        return 0;
    }
    adamcore_get_machine_state(core, &mem_ctrl, &net_ctrl, &game);
    adamcore_sgm_state(core, &sgm);
    cart = coleco_host_cart();
    have_cart = cart != NULL;
    memset(&info, 0, sizeof(info));
    if (have_cart) coleco_cart_get_info(cart, &info);
    colecodebug_unlock(dbg);

    if ((mem_ctrl & 3) == 3) {
        map_add(&map, 0x0000, 0x1FFF, 1, sgm.bios_off, sgm.bios_off ? "SGM RAM (over the BIOS)" : "OS7 BIOS ROM");
        if (sgm.ram_en) {
            map_add(&map, 0x2000, 0x7FFF, 1, 1, "SGM RAM");
        } else {
            map_add(&map, 0x2000, 0x5FFF, 0, 0, "open bus (unpopulated)");
            map_add(&map, 0x6000, 0x63FF, 1, 1, "Console RAM (1K)");
            map_add(&map, 0x6400, 0x7FFF, 1, 1, "Console RAM (mirror of $6000-$63FF)");
        }
    } else {
        snprintf(text, sizeof(text), "RAM (memory control %02X)", mem_ctrl);
        map_add(&map, 0x0000, 0x7FFF, 1, 1, text);
    }

    switch ((mem_ctrl >> 2) & 3) {
    case 3:
        if (!have_cart) {
            map_add(&map, 0x8000, 0xFFFF, 0, 0, "Cartridge (none)");
        } else if (info.kind == COLMAP_MEGACART || info.kind == COLMAP_ACTIVISION) {
            snprintf(text, sizeof(text), "Cartridge (%s, fixed bank)", colmap_kindname(info.kind));
            map_add(&map, 0x8000, 0xBFFF, 1, 0, text);
            snprintf(text, sizeof(text), "Cartridge (%s, bank %u)", colmap_kindname(info.kind),
                     (unsigned)info.bank);
            map_add(&map, 0xC000, 0xFFFF, 1, 0, text);
        } else {
            snprintf(text, sizeof(text), "Cartridge (%s, %u bytes)", colmap_kindname(info.kind),
                     (unsigned)info.size);
            map_add(&map, 0x8000, 0xFFFF, 1, 0, text);
        }
        break;
    case 1:
        map_add(&map, 0x8000, 0xFFFF, 0, 0, "open bus (expansion ROM socket empty)");
        break;
    default:
        map_add(&map, 0x8000, 0xFFFF, 1, 1, "RAM");
        break;
    }
    return map.count;
}

/* ---- Sound --------------------------------------------------------------------------- */

int colecodebug_sound_get(colecodebug *dbg, colecodebug_sound *out)
{
    adamcore *core;
    adamcore_sn_state sn;
    adamcore_sgm_info sgm;
    int idx;

    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!dbg) return 0;
    colecodebug_lock(dbg);
    core = colecodebug_core_locked(dbg);
    if (!core) {
        colecodebug_unlock(dbg);
        return 0;
    }
    adamcore_psg_sn_state(core, &sn);
    adamcore_sgm_state(core, &sgm);
    colecodebug_unlock(dbg);

    out->sn.present = 1;
    out->sn.latched_reg = sn.latched_reg;
    for (idx = 0; idx < 3; idx++) {
        out->sn.period[idx] = sn.period[idx] ? sn.period[idx] : 1024;
        out->sn.freq[idx] = 3579545.0 / (32.0 * out->sn.period[idx]);
    }
    for (idx = 0; idx < 4; idx++) {
        out->sn.atten[idx] = sn.atten[idx] & 0x0F;
        out->sn.level[idx] = 15 - out->sn.atten[idx];
    }
    out->sn.noise_ctrl = sn.noise_ctrl;
    out->sn.noise_white = (sn.noise_ctrl & 4) != 0;
    out->sn.noise_rate = sn.noise_ctrl & 3;
    out->sn.noise_hz = out->sn.noise_rate == 3 ? out->sn.freq[2]
                                               : 3579545.0 / (512.0 * (double)(1 << out->sn.noise_rate));

    if (sgm.configured) {
        colecodebug_ay *ay = &out->ay;
        ay->present = 1;
        ay->addr = sgm.ay_addr;
        memcpy(ay->reg, sgm.ay_regs, sizeof(ay->reg));
        for (idx = 0; idx < 3; idx++) {
            const int period = ay->reg[idx * 2] | ((ay->reg[idx * 2 + 1] & 0x0F) << 8);
            ay->period[idx] = period;
            ay->freq[idx] = 111860.8 / (period ? period : 1);
            ay->tone_on[idx] = !((ay->reg[7] >> idx) & 1);
            ay->noise_on[idx] = !((ay->reg[7] >> (3 + idx)) & 1);
            ay->level[idx] = ay->reg[8 + idx] & 0x0F;
            ay->envelope[idx] = (ay->reg[8 + idx] & 0x10) != 0;
        }
        ay->noise_period = ay->reg[6] & 0x1F;
        ay->env_period = ay->reg[11] | (ay->reg[12] << 8);
        ay->env_shape = ay->reg[13] & 0x0F;
    }
    return 1;
}
