/*
 * external_test -- "Use external FujiNet": the session starts no runtime of
 * its own and its cartridge dials the address in the settings, here a
 * stand-in listener on a spare loopback port. A bad address refuses to
 * start. Needs a BIOS: argv[1] is a directory holding OS7.rom, else the
 * test SKIPs (77) like the other machine-running tests.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
int main(void)
{
    printf("SKIP: POSIX sockets only\n");
    return 77;
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "colecosession.h"

static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

int main(int argc, char **argv)
{
    char cfg[512], data[512], addr[64], bios[600];
    colecosession_paths paths;
    colecosession_start_opts opts;
    struct sockaddr_in sin;
    socklen_t len = sizeof sin;
    int lsock, conn = -1;
    colecosession *s;

    lsock = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (lsock < 0 || bind(lsock, (struct sockaddr *)&sin, sizeof sin) != 0 ||
        listen(lsock, 1) != 0 ||
        getsockname(lsock, (struct sockaddr *)&sin, &len) != 0) {
        printf("SKIP: no loopback listener\n");
        return 77;
    }
    snprintf(addr, sizeof addr, "127.0.0.1:%d", (int)ntohs(sin.sin_port));

    snprintf(bios, sizeof bios, "%s/OS7.rom", argc > 1 ? argv[1] : "");
    if (argc < 2 || access(bios, R_OK) != 0) {
        printf("SKIP: no OS7.rom\n");
        return 77;
    }
    snprintf(cfg, sizeof cfg, "/tmp/coleco-ext-%d/config", (int)getpid());
    snprintf(data, sizeof data, "/tmp/coleco-ext-%d/data", (int)getpid());
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    s = colecosession_new(&paths);
    if (!s || colecosession_import_bios(s, bios) < 0) return 1;

    colecosession_default_opts(s, &opts);
    opts.cart_path = NULL;
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    opts.external_fujinet = 1;

    opts.boip_connect = "not an address";
    check(colecosession_start(s, &opts) != 0, "a bad address refuses to start");
    check(!colecosession_is_running(s), "nothing runs after the refusal");

    opts.boip_connect = addr;
    check(colecosession_start(s, &opts) == 0, "starts with an external FujiNet");
    check(colecosession_fujinet_external(s), "reports external");
    check(!colecosession_fujinet_running(s), "no built-in runtime started");
    check(strcmp(colecosession_boip_endpoint(s), addr) == 0, "endpoint is the address");
    check(colecosession_fujinet_webui_url(s)[0] == '\0', "no web UI URL");

    {
        struct pollfd pfd = { lsock, POLLIN, 0 };
        int waited = 0;
        while (waited < 15000 && poll(&pfd, 1, 100) == 0)
            waited += 100;
        if (pfd.revents & POLLIN)
            conn = accept(lsock, NULL, NULL);
        check(conn >= 0, "the cartridge dialed the stand-in listener");
    }

    if (conn >= 0) close(conn);
    colecosession_stop(s);
    check(!colecosession_fujinet_external(s), "not external once stopped");
    colecosession_free(s);
    close(lsock);
    {
        char cmd[600];
        snprintf(cmd, sizeof cmd, "rm -rf '/tmp/coleco-ext-%d'", (int)getpid());
        if (system(cmd) != 0) { /* best effort */ }
    }
    return failures ? 1 : 0;
}
#endif
