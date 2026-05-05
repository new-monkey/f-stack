/*
 * Copyright (C) 2017-2021 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * ff_netctl – F-Stack Network Configuration CLI Client
 *
 * A pure Linux utility (no DPDK, no FreeBSD compat headers) that connects to
 * ff_netd via a Unix domain socket, sends a command string, and prints the
 * response.
 *
 * Usage:
 *   ff_netctl [-s socket] <command> [args ...]
 *
 * Commands:
 *   addr show [<ifname>]
 *   addr add  <ifname> <ip> <netmask>
 *   addr del  <ifname> <ip>
 *   link show [<ifname>]
 *   link set  <ifname> up|down
 *   route show
 *   route add <dest/prefix> [gw] <gw>
 *   route del <dest/prefix>
 *   status
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#define FF_NETD_SOCKET_DEFAULT  "/var/run/ff_netd.sock"
#define RESP_BUF_SIZE           (256 * 1024)

static void
usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-s socket] <command> [args ...]\n"
        "\n"
        "Options:\n"
        "  -s socket   Path to ff_netd socket (default: %s)\n"
        "\n"
        "Commands:\n"
        "  addr show [<ifname>]\n"
        "  addr add  <ifname> <ip> <netmask>\n"
        "  addr del  <ifname> <ip>\n"
        "  link show [<ifname>]\n"
        "  link set  <ifname> up|down\n"
        "  route show\n"
        "  route add <dest/prefix> [gw] <gw>\n"
        "  route del <dest/prefix>\n"
        "  status\n",
        prog, FF_NETD_SOCKET_DEFAULT);
}

int
main(int argc, char *argv[])
{
    const char *socket_path = FF_NETD_SOCKET_DEFAULT;
    int arg_start = 1;

    /* Parse optional -s <socket> flag */
    if (argc >= 3 && strcmp(argv[1], "-s") == 0) {
        socket_path = argv[2];
        arg_start   = 3;
    }

    if (arg_start >= argc) {
        usage(argv[0]);
        return 1;
    }

    /* ------------------------------------------------------------------
     * Build the command string from the remaining arguments.
     * ------------------------------------------------------------------ */
    char cmd[512] = "";
    for (int i = arg_start; i < argc; i++) {
        if (i > arg_start)
            strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 1);
    }
    /* Append newline so ff_netd can detect end-of-line */
    strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);

    /* ------------------------------------------------------------------
     * Connect to ff_netd.
     * ------------------------------------------------------------------ */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "ff_netctl: socket: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr,
                (socklen_t)sizeof(addr)) < 0) {
        fprintf(stderr, "ff_netctl: connect(%s): %s\n",
                socket_path, strerror(errno));
        fprintf(stderr,
                "  Is ff_netd running?  "
                "Start it with: ff_netd [-s %s]\n", socket_path);
        close(fd);
        return 1;
    }

    /* ------------------------------------------------------------------
     * Send the command.
     * ------------------------------------------------------------------ */
    if (send(fd, cmd, strlen(cmd), 0) < 0) {
        fprintf(stderr, "ff_netctl: send: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    /* ------------------------------------------------------------------
     * Read the response until the server closes the connection.
     * ------------------------------------------------------------------ */
    char *resp = malloc(RESP_BUF_SIZE);
    if (!resp) {
        fprintf(stderr, "ff_netctl: out of memory\n");
        close(fd);
        return 1;
    }

    size_t  total = 0;
    ssize_t n;
    while (total < RESP_BUF_SIZE - 1 &&
           (n = recv(fd, resp + total, RESP_BUF_SIZE - 1 - total, 0)) > 0)
        total += (size_t)n;

    close(fd);
    resp[total] = '\0';

    /* ------------------------------------------------------------------
     * Parse and print the response.
     *
     * Protocol:
     *   Success: "+OK\n<content>"
     *   Error:   "-ERR <message>\n"
     * ------------------------------------------------------------------ */
    int ret = 0;

    if (total == 0) {
        fprintf(stderr, "ff_netctl: no response from ff_netd\n");
        free(resp);
        return 1;
    }

    if (strncmp(resp, "+OK\n", 4) == 0) {
        /* Multi-line success: print everything after the "+OK\n" header */
        const char *content = resp + 4;
        if (*content)
            printf("%s", content);
    } else if (strncmp(resp, "+OK ", 4) == 0) {
        /* Single-line success: print message after "+OK " */
        const char *msg = resp + 4;
        if (*msg)
            printf("%s", msg);
    } else if (strncmp(resp, "-ERR ", 5) == 0) {
        /* Error: print message after "-ERR " to stderr */
        fprintf(stderr, "Error: %s", resp + 5);
        ret = 1;
    } else {
        /* Unexpected format – print as-is */
        printf("%s", resp);
    }

    free(resp);
    return ret;
}
