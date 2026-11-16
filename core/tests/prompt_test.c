/*
 * prompt_test -- the debugger's command line (core/debugger/prompt.c) on an
 * engine with no machine behind it: the commands that only touch the engine's
 * own state (expressions, breakpoints with conditions, the watch list,
 * symbols, help, completion) and what is refused. Anything that needs the
 * machine is engine_test's.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "colecodebug.h"
#include "colecomemtools.h"

static int failures;
static colecodebug *g_dbg;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "prompt_test: " __VA_ARGS__); fputc('\n', stderr); failures = 1; } } while (0)

static const char *run(const char *line)
{
    static char out[8192];
    colecodebug_command(g_dbg, line, out, sizeof out);
    return out;
}

static int has(const char *line, const char *want)
{
    const char *got = run(line);
    if (!strstr(got, want)) {
        fprintf(stderr, "prompt_test: \"%s\" printed \"%s\", want \"%s\"\n", line, got, want);
        failures = 1;
        return 0;
    }
    return 1;
}

static char section_seen[32];

static const char *video_stub(const char *section)
{
    snprintf(section_seen, sizeof section_seen, "%s", section);
    return "Display 256x192\n";
}

int main(void)
{
    char text[2048];
    colecodebug_breakpoint list[16];
    int count;

    g_dbg = colecosession_debugger(NULL);
    if (!g_dbg) {
        fprintf(stderr, "prompt_test: no debugger\n");
        return 1;
    }

    /* print: every notation, with the binary form */
    has("print $6000 + 16", "$6010  24592  %0110000000010000");
    has("print 0x10 * 3 - 1", "$002F  47");
    has("print %1010 | 1", "$000B  11");
    has("p (1+2)*3", "$0009  9");
    has("print nosuchlabel_zz", "error: unknown name 'nosuchlabel_zz'");
    has("print 1 +", "error:");
    has("print 1/0", "division by zero");
    /* with no machine the registers read 0 and memory is open bus */
    has("print hl + 5", "$0005  5");
    has("print [$6000]", "$00FF  255");

    /* labels, user and built in */
    has("label $4010 MYSPOT", "$4010 = MYSPOT");
    has("print MYSPOT + 1", "$4011");
    has("symbols MYSPOT", "$4010  MYSPOT");
    has("symbols zzzzqqqq", "no labels match");
    CHECK(colecodebug_label_address(g_dbg, "MYSPOT") == 0x4010, "label not stored");
    has("label $4010", "usage");
    has("symbols PUT_VRAM", "PUT_VRAM");

    /* breakpoints: every kind, ranges, conditions */
    has("break $4000", "breakpoint 1 set");
    has("bpw $6000-$63FF value != old", "breakpoint 2 set");
    has("bpr MYSPOT", "breakpoint 3 set");
    has("bpa $6100 value == 5 && addr == $6100", "breakpoint 4 set");
    has("bpin $FC", "breakpoint 5 set");
    has("bpout $A0-$A1 value >= $80", "breakpoint 6 set");
    has("break $4020 a == 1", "breakpoint 7 set");
    has("break $4030 a == 1 && hl > $6000", "breakpoint 8 set");
    count = colecodebug_breakpoint_list(g_dbg, list, 16);
    CHECK(count == 8, "%d breakpoints, want 8", count);
    CHECK(list[3].type == COLECODEBUG_BP_ACCESS && list[3].start == 0x6100, "the access entry");
    CHECK(list[1].type == COLECODEBUG_BP_WRITE && list[1].end == 0x63FF, "the write range");
    CHECK(!strcmp(list[1].condition, "value != old"), "the condition is kept as typed: \"%s\"", list[1].condition);
    has("breaks", "4 access $6100 on hits 0  if value == 5 && addr == $6100");
    has("break $4040 nosuch == 1", "error:");
    has("break $4040 {bad", "error:");
    has("break $4040 a ==", "error:");
    has("bpin $100", "error: ports are 0-255");
    has("bpa", "usage");
    CHECK(colecodebug_breakpoint_list(g_dbg, list, 16) == 8, "a refused breakpoint was added");

    has("disable 2", "disabled 2");
    colecodebug_breakpoint_list(g_dbg, list, 16);
    CHECK(!list[1].enabled, "disable");
    has("enable #2", "enabled 2");
    has("hits clear", "hit counts cleared");
    has("delete $4000", "deleted 1");
    has("delete 3", "deleted 3");
    has("delete 99", "no such breakpoint");
    CHECK(colecodebug_breakpoint_list(g_dbg, list, 16) == 6, "delete left %d",
          colecodebug_breakpoint_list(g_dbg, list, 16));
    /* a bare hex address is hex unless it is a label */
    has("bpw 6000", "breakpoint");
    has("bpin fc", "breakpoint");
    has("break 4a3c", "breakpoint");
    count = colecodebug_breakpoint_list(g_dbg, list, 16);
    CHECK(list[count - 1].start == 0x4A3C && list[count - 2].start == 0xFC && list[count - 3].start == 0x6000,
          "bare hex addresses: $%04X $%04X $%04X", list[count - 3].start, list[count - 2].start,
          list[count - 1].start);
    has("label $5000 beef", "$5000 = beef");
    has("break beef", "breakpoint");
    count = colecodebug_breakpoint_list(g_dbg, list, 16);
    CHECK(list[count - 1].start == 0x5000, "a label wins over bare hex");
    has("clear", "all breakpoints cleared");
    has("breaks", "no breakpoints");

    /* the older single-address API shares the table */
    colecodebug_bp_set(g_dbg, 0x1234);
    CHECK(colecodebug_bp_is_set(g_dbg, 0x1234) && colecodebug_breakpoint_check(g_dbg, 0x1234), "legacy set");
    has("breaks", "exec $1234 on hits 0");
    colecodebug_bp_clear(g_dbg, 0x1234);
    CHECK(!colecodebug_bp_is_set(g_dbg, 0x1234), "legacy clear");

    /* the watch list */
    colecowatch_clear();
    has("watch", "no watches");
    has("watch $6000", "$6000");
    has("watch MYSPOT", "MYSPOT");
    CHECK(colecowatch_count() == 2, "two watches expected");
    has("unwatch 0", "");
    CHECK(colecowatch_count() == 1 && !strcmp(colecowatch_text(0), "MYSPOT"), "unwatch 0 removed the wrong one");
    has("unwatch all", "");
    CHECK(colecowatch_count() == 0, "unwatch all");
    has("unwatch", "unwatch <n|all>");

    /* a machine-less engine says so rather than crashing */
    has("find 3E 00", "stop the machine first");
    has("find text COL", "stop the machine first");
    has("find zz", "find [hex]");
    has("set a 5", "the machine is running");
    has("set nosuch 5", "unknown register");
    has("set a", "usage");
    has("step", "not stopped");
    has("over", "not stopped");
    has("map", "not running");
    has("io", "not running");
    has("cart", "no machine");
    has("sound", "no machine");
    has("video", "No machine is running");
    has("regs", "AF=0000");
    has("frobnicate", "unknown command");

    /* the video hook is the lead's: a stub answers the command */
    {
        colecodebug_set_video_text(video_stub);
        has("video decoded", "Display 256x192");
        CHECK(!strcmp(section_seen, "decoded"), "the section reached the hook");
        colecodebug_set_video_text(NULL);
    }

    /* help and completion */
    has("help", "bpa <addr>");
    has("HELP", "bpa <addr>");
    has("?", "video [section]");
    CHECK(colecodebug_completions(g_dbg, "wat", text, sizeof text) >= 1 && strstr(text, "watch") != NULL, "completion of wat");
    CHECK(colecodebug_completions(g_dbg, "bp", text, sizeof text) == 5,
          "bp completes five commands (bpr bpw bpa bpin bpout), got %s", text);
    CHECK(colecodebug_completions(g_dbg, "print PUT_VR", text, sizeof text) >= 1 && strstr(text, "PUT_VRAM") != NULL,
          "label completion");
    CHECK(colecodebug_completions(g_dbg, "print h", text, sizeof text) >= 1 && strstr(text, "hl") != NULL,
          "register completion");
    CHECK(colecodebug_completions(g_dbg, "save d", text, sizeof text) == 1 && strstr(text, "dis") != NULL, "save kinds");
    CHECK(colecodebug_completions(g_dbg, "vid", text, sizeof text) >= 1 && strstr(text, "video") != NULL, "video completes");

    if (!failures) printf("prompt_test: ok\n");
    return failures;
}
