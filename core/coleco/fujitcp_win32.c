/*
 * fujitcp_win32 -- the Winsock twin of the staged POSIX fujitcp.c.
 *
 * Compiled INSTEAD of core/coleco/fuji-generated/fujitcp.c on Windows, against
 * the SAME staged fujitcp.h, so the cart device and every caller see one
 * interface. The staged file stays a verbatim copy of the firmware's own; this
 * is the port, kept beside the device it serves rather than patched into the
 * staging tree, so `git diff` against fujinet-firmware stays empty.
 *
 * Deliberately a line-for-line transposition rather than a rewrite. The
 * behaviour that matters is subtle and was paid for elsewhere:
 *
 *   - Walk EVERY getaddrinfo result. "localhost" usually resolves to ::1
 *     first, while fujinet-pc's BoIP listener binds 127.0.0.1 only.
 *   - RX_RAW_MAX must hold a SLIP-encoded 512-byte DBC push frame. Undersizing
 *     it silently truncates every ROM push -- the 1088 trap, paid for once on
 *     the Intellivision.
 *   - A push frame consumed mid-transaction RESTARTS the deadline rather than
 *     counting down: it proves the link is alive.
 *
 * Winsock differences, all of them mechanical:
 *   SOCKET is unsigned and INVALID_SOCKET is not -1; closesocket, not close;
 *   recv takes a char*; setsockopt takes a const char*; there is no
 *   MSG_DONTSIGNAL/MSG_NOSIGNAL (Windows never raises SIGPIPE) and no
 *   MSG_DONTWAIT (a zero-timeout select before each byte does the same job);
 *   and WSAStartup has to happen before anything else.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fujitcp.h"
#include "fujimail.h"
#include "fuji_mailbox.h"

#define RX_RAW_MAX 1088

static SOCKET sock = INVALID_SOCKET;
static int wsa_started;

bool fujitcp_active(void)
{
    return sock != INVALID_SOCKET;
}

int fujitcp_init(const char *hostport)
{
    char host[256], *colon;
    int port = 9995;
    BOOL one = TRUE;
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];

    if (!wsa_started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "fujinet: WSAStartup failed\n");
            return -1;
        }
        wsa_started = 1;
    }

    if (hostport == NULL)
        hostport = getenv("FUJINET_TCP");
    if (hostport == NULL)
        hostport = "127.0.0.1:9995";
    snprintf(host, sizeof host, "%s", hostport);
    colon = strrchr(host, ':');
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
    }
    if (host[0] == '\0')
        snprintf(host, sizeof host, "127.0.0.1");
    snprintf(portstr, sizeof portstr, "%d", port);

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL) {
        fprintf(stderr, "fujinet: cannot resolve %s:%d\n", host, port);
        return -1;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == INVALID_SOCKET)
            continue;
        if (connect(sock, ai->ai_addr, (int)ai->ai_addrlen) == 0)
            break;
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "fujinet: cannot connect to %s:%d (WSA %d)\n",
                host, port, WSAGetLastError());
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    fprintf(stderr, "fujinet: connected to %s:%d\n", host, port);
    return 0;
}

void fujitcp_close(void)
{
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
}

/* Read one complete SLIP frame (two 0xC0 delimiters) or time out. */
static fb_status_t read_frame(uint8_t *buf, size_t cap, size_t *out_len,
                              int secs)
{
    size_t n = 0;
    int ends = 0;
    DWORD deadline = GetTickCount() + (DWORD)secs * 1000u;

    for (;;) {
        fd_set rf;
        struct timeval tv;
        DWORD now = GetTickCount();
        long remain_ms;

        /* GetTickCount wraps after ~49 days; the signed difference keeps
         * that from turning into a colossal timeout at the wrap. */
        remain_ms = (long)(deadline - now);
        if (remain_ms <= 0)
            return FB_ETIMEOUT;
        tv.tv_sec = remain_ms / 1000;
        tv.tv_usec = (remain_ms % 1000) * 1000;

        FD_ZERO(&rf);
        FD_SET(sock, &rf);
        /* Winsock ignores the first argument; passing 0 is idiomatic. */
        if (select(0, &rf, NULL, NULL, &tv) <= 0)
            return FB_ETIMEOUT;

        while (n < cap) {
            char c;
            struct timeval zero = { 0, 0 };
            fd_set one;
            int r;

            /* No MSG_DONTWAIT on Winsock: a zero-timeout select before each
             * byte gives the same "drain what is here, then go back to the
             * outer wait" behaviour the POSIX version relies on. */
            FD_ZERO(&one);
            FD_SET(sock, &one);
            if (select(0, &one, NULL, NULL, &zero) <= 0)
                break;

            r = recv(sock, &c, 1, 0);
            if (r <= 0)
                break;
            buf[n++] = (uint8_t)c;
            if ((uint8_t)c == 0xC0 && ++ends == 2) {
                *out_len = n;
                return FB_OK;
            }
        }
        if (n >= cap)
            return FB_ETOOBIG;
    }
}

fb_status_t fujitcp_transact(uint8_t device, uint8_t command,
                             const fb_param_t *params, unsigned nparams,
                             const uint8_t *payload, uint16_t payload_len,
                             uint32_t timeout_ms, fb_reply_t *reply)
{
    static uint8_t req[FN_TX_MAX + 64];
    static uint8_t raw[RX_RAW_MAX];
    size_t reqlen, rawlen;
    fb_status_t st;
    int secs = (int)((timeout_ms + 999) / 1000);

    if (!fujitcp_active())
        return FB_ENOLINK;

    reqlen = fujibus_build_request(device, command, params, nparams,
                                   payload, payload_len, req, sizeof req);
    if (reqlen == 0)
        return FB_ETOOBIG;
    if (send(sock, (const char *)req, (int)reqlen, 0) != (int)reqlen)
        return FB_ENOLINK;

    for (;;) {
        st = read_frame(raw, sizeof raw, &rawlen, secs);
        if (st != FB_OK)
            return st;
        if (!fujibus_parse_reply(raw, rawlen, reply))
            return FB_EBADFRAME;
        /* Push frames arrive interleaved with the reply we are waiting for;
         * consuming one proves the link is alive, so the deadline restarts
         * rather than counting down. */
        if (!fujimail_inbound(reply))
            return FB_OK;
    }
}

void fujitcp_send_bare(uint8_t device, uint8_t command,
                       const uint8_t *payload, uint16_t payload_len)
{
    uint8_t frame[64];
    size_t n = fujibus_build_request(device, command, NULL, 0,
                                     payload, payload_len, frame,
                                     sizeof frame);

    if (n && sock != INVALID_SOCKET)
        send(sock, (const char *)frame, (int)n, 0);
}
