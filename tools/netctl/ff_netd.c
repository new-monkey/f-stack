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
 * ff_netd - F-Stack Network Configuration Daemon
 *
 * Runs as a DPDK secondary process. Listens on a Unix domain socket and
 * forwards network-configuration requests to every F-Stack worker process via
 * the DPDK ring IPC mechanism, then returns the aggregated result.
 *
 * Supported commands (received as a single line over the socket):
 *
 *   addr show [<ifname>]
 *   addr add  <ifname> <ip> <netmask>
 *   addr del  <ifname> <ip>
 *   link set  <ifname> up|down
 *   route show
 *   route add <dest/prefix> gw <gw>
 *   route del <dest/prefix>
 *   status
 *
 * Response protocol (text, written then connection closed):
 *   Success: "+OK\n<content lines>\n"
 *   Error:   "-ERR <message>\n"
 *
 * Usage: ff_netd [-s /path/to/socket]
 *   Default socket: /var/run/ff_netd.sock
 *   ff_netd must be started AFTER the F-Stack worker processes.
 */

/* -----------------------------------------------------------------------
 * Standard headers first (before any compat overrides are pulled in)
 * ---------------------------------------------------------------------- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>

/* -----------------------------------------------------------------------
 * FreeBSD compat headers.
 * Including <sys/ioctl.h> from the compat path pulls in sys/ioccom.h which
 * defines:  #define ioctl(a,b,c)  ioctl_va((a),(b),(c),0)
 * All subsequent ioctl() calls are therefore redirected to ioctl_va() which
 * routes them through the DPDK ring IPC to the target worker process.
 * ---------------------------------------------------------------------- */
#include <sys/socket.h>         /* AF_UNIX, SOCK_STREAM, struct sockaddr   */
#include <sys/ioctl.h>          /* ioctl -> ioctl_va via macro             */
#include <sys/sockio.h>         /* SIOCAIFADDR, SIOCDIFADDR, SIOCG/SIFFLAGS */
#include <sys/sysctl.h>         /* CTL_NET                                 */
#include <net/if.h>             /* struct ifreq, IFF_UP, IFNAMSIZ          */
#include <net/if_dl.h>          /* struct sockaddr_dl                      */
#include <net/route.h>          /* struct rt_msghdr, RTM_*, RTA_*, RTAX_*  */
#include <netinet/in.h>         /* struct sockaddr_in, AF_INET             */
#include <netinet/in_var.h>     /* struct in_aliasreq                      */
#include <ifaddrs.h>            /* getifaddrs / freeifaddrs                */
#include <arpa/inet.h>          /* inet_aton / inet_ntoa                   */

/* -----------------------------------------------------------------------
 * DPDK & F-Stack IPC
 * ---------------------------------------------------------------------- */
#include <rte_ring.h>
#include "ff_ipc.h"
#include "rtioctl.h"

/* -----------------------------------------------------------------------
 * Linux-compatible Unix-domain socket address.
 *
 * The compat sys/un.h uses the BSD layout:
 *   { uint8_t sun_len;  sa_family_t sun_family;  char sun_path[104]; }
 * The Linux kernel expects:
 *   { uint16_t sa_family;  char sun_path[108]; }
 * (The address-family field must occupy the first two bytes as a 16-bit
 * little-endian integer on x86/x86-64.)
 *
 * We define our own struct to avoid this BSD/Linux layout mismatch when
 * calling the real Linux socket()/bind()/accept() syscalls.
 * ---------------------------------------------------------------------- */
struct unix_addr {
    unsigned short  sa_family;   /* AF_UNIX = 1, same on Linux & BSD */
    char            sa_path[108];
};

/* -----------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */
#define FF_NETD_SOCKET_DEFAULT  "/var/run/ff_netd.sock"
#define MAX_PROCS               64
#define MAX_CMD_LEN             512
#define MAX_RESP_BUF            (256 * 1024)

/* Alignment macro for routing socket message sockaddrs */
#define RT_SALIGN               (sizeof(long) - 1)
#define RT_SA_RLEN(sa) \
    ((sa)->sa_len ? (((sa)->sa_len + RT_SALIGN) & ~RT_SALIGN) : \
                    (RT_SALIGN + 1))

/* -----------------------------------------------------------------------
 * Globals
 * ---------------------------------------------------------------------- */
static int  nb_procs    = 0;
static int  server_fd   = -1;
static char socket_path[256];

/* -----------------------------------------------------------------------
 * Worker-process discovery
 * Probe ff_msg_ring_in_N for N = 0, 1, … until the ring does not exist.
 * ---------------------------------------------------------------------- */
static int
discover_nb_procs(void)
{
    int nb = 0;
    char name[RTE_RING_NAMESIZE];

    for (int i = 0; i < MAX_PROCS; i++) {
        snprintf(name, sizeof(name), "%s%d", FF_MSG_RING_IN, i);
        if (rte_ring_lookup(name) == NULL)
            break;
        nb++;
    }
    return nb;
}

/* -----------------------------------------------------------------------
 * Broadcast an ioctl to all worker processes.
 * Returns 0 if every worker succeeded, -1 on the first failure (errbuf
 * is populated with the error description).
 * ---------------------------------------------------------------------- */
static int
broadcast_ioctl(unsigned long cmd, void *data, int af,
                char *errbuf, size_t errsz)
{
    for (int i = 0; i < nb_procs; i++) {
        ff_set_proc_id(i);
        if (ioctl_va(0, cmd, data, 1, af) < 0) {
            if (errbuf)
                snprintf(errbuf, errsz,
                         "proc %d: %s", i, strerror(errno));
            return -1;
        }
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Broadcast a routing socket message to all worker processes.
 * ---------------------------------------------------------------------- */
static int
broadcast_rtmsg(void *msg, size_t len, char *errbuf, size_t errsz)
{
    for (int i = 0; i < nb_procs; i++) {
        ff_set_proc_id(i);
        if (rtioctl((char *)msg, (unsigned)len, 0) < 0) {
            if (errbuf)
                snprintf(errbuf, errsz,
                         "proc %d: %s", i, strerror(errno));
            return -1;
        }
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * addr show [ifname]
 * Queries proc 0 via getifaddrs (which uses sysctl IPC internally).
 * Writes formatted output to out[0..outlen).
 * ---------------------------------------------------------------------- */
static void
cmd_addr_show(const char *ifname, char *out, size_t outlen)
{
    ff_set_proc_id(0);

    struct ifaddrs *ifa_list;
    if (getifaddrs(&ifa_list) < 0) {
        snprintf(out, outlen, "getifaddrs: %s\n", strerror(errno));
        return;
    }

    size_t pos  = 0;
    const char *last_name = NULL;

    for (struct ifaddrs *ifa = ifa_list; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;
        if (ifname && strcmp(ifa->ifa_name, ifname) != 0)
            continue;

        int af = ifa->ifa_addr->sa_family;

        /* Print interface header once per interface */
        if (af == AF_LINK) {
            last_name = ifa->ifa_name;
            pos += snprintf(out + pos, outlen - pos,
                "%-12s flags=%#x\n",
                ifa->ifa_name, (unsigned)ifa->ifa_flags);
        } else if (af == AF_INET) {
            struct sockaddr_in *sin  =
                (struct sockaddr_in *)ifa->ifa_addr;
            struct sockaddr_in *mask =
                (struct sockaddr_in *)ifa->ifa_netmask;

            /* Ensure we printed the interface header */
            if (last_name == NULL || strcmp(last_name, ifa->ifa_name) != 0) {
                last_name = ifa->ifa_name;
                pos += snprintf(out + pos, outlen - pos,
                    "%-12s flags=%#x\n",
                    ifa->ifa_name, (unsigned)ifa->ifa_flags);
            }

            char ip_s[INET_ADDRSTRLEN];
            char nm_s[INET_ADDRSTRLEN];
            /* Use inet_ntop to avoid clobbering the static buffer that
             * inet_ntoa reuses across consecutive calls. */
            inet_ntop(AF_INET, &sin->sin_addr, ip_s, sizeof(ip_s));
            if (mask)
                inet_ntop(AF_INET, &mask->sin_addr, nm_s, sizeof(nm_s));
            else
                snprintf(nm_s, sizeof(nm_s), "0.0.0.0");

            pos += snprintf(out + pos, outlen - pos,
                "  inet %-15s netmask %s", ip_s, nm_s);

            if (ifa->ifa_broadaddr) {
                struct sockaddr_in *brd =
                    (struct sockaddr_in *)ifa->ifa_broadaddr;
                char brd_s[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &brd->sin_addr, brd_s, sizeof(brd_s));
                pos += snprintf(out + pos, outlen - pos,
                    " broadcast %s", brd_s);
            }
            pos += snprintf(out + pos, outlen - pos, "\n");
        }

        /* Safety margin: stop when the buffer is nearly full */
        if (pos >= outlen - 128)
            break;
    }

    freeifaddrs(ifa_list);
}

/* -----------------------------------------------------------------------
 * addr add <ifname> <ip> <netmask>
 * Broadcasts SIOCAIFADDR to every worker process.
 * ---------------------------------------------------------------------- */
static int
cmd_addr_add(const char *ifname, const char *ip, const char *netmask,
             char *errbuf, size_t errsz)
{
    struct in_aliasreq req;
    memset(&req, 0, sizeof(req));
    strncpy(req.ifra_name, ifname, IFNAMSIZ - 1);

    req.ifra_addr.sin_family = AF_INET;
    req.ifra_addr.sin_len    = sizeof(struct sockaddr_in);
    if (!inet_aton(ip, &req.ifra_addr.sin_addr)) {
        snprintf(errbuf, errsz, "invalid IP: %s", ip);
        return -1;
    }

    req.ifra_mask.sin_family = AF_INET;
    req.ifra_mask.sin_len    = sizeof(struct sockaddr_in);
    if (!inet_aton(netmask, &req.ifra_mask.sin_addr)) {
        snprintf(errbuf, errsz, "invalid netmask: %s", netmask);
        return -1;
    }

    /* Derive broadcast: addr | ~mask */
    uint32_t addr_h = ntohl(req.ifra_addr.sin_addr.s_addr);
    uint32_t mask_h = ntohl(req.ifra_mask.sin_addr.s_addr);
    req.ifra_broadaddr.sin_family            = AF_INET;
    req.ifra_broadaddr.sin_len               = sizeof(struct sockaddr_in);
    req.ifra_broadaddr.sin_addr.s_addr       = htonl(addr_h | ~mask_h);

    return broadcast_ioctl(SIOCAIFADDR, &req, AF_INET, errbuf, errsz);
}

/* -----------------------------------------------------------------------
 * addr del <ifname> <ip>
 * Broadcasts SIOCDIFADDR to every worker process.
 * ---------------------------------------------------------------------- */
static int
cmd_addr_del(const char *ifname, const char *ip,
             char *errbuf, size_t errsz)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    sin->sin_len    = sizeof(struct sockaddr_in);
    if (!inet_aton(ip, &sin->sin_addr)) {
        snprintf(errbuf, errsz, "invalid IP: %s", ip);
        return -1;
    }

    return broadcast_ioctl(SIOCDIFADDR, &ifr, AF_INET, errbuf, errsz);
}

/* -----------------------------------------------------------------------
 * link set <ifname> up|down
 * Reads current flags from proc 0, then broadcasts SIOCSIFFLAGS.
 * ---------------------------------------------------------------------- */
static int
cmd_link_set(const char *ifname, int up, char *errbuf, size_t errsz)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    /* Read current flags from proc 0 */
    ff_set_proc_id(0);
    if (ioctl_va(0, SIOCGIFFLAGS, &ifr, 1, AF_INET) < 0) {
        snprintf(errbuf, errsz, "SIOCGIFFLAGS: %s", strerror(errno));
        return -1;
    }

    if (up)
        ifr.ifr_flags |=  IFF_UP;
    else
        ifr.ifr_flags &= ~IFF_UP;

    return broadcast_ioctl(SIOCSIFFLAGS, &ifr, AF_INET, errbuf, errsz);
}

/* -----------------------------------------------------------------------
 * route show
 * Queries the IPv4 routing table from proc 0 via sysctl(NET_RT_DUMP).
 * ---------------------------------------------------------------------- */
static void
cmd_route_show(char *out, size_t outlen)
{
    int mib[6] = { CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_DUMP, 0 };
    size_t needed;

    ff_set_proc_id(0);

    if (sysctl(mib, 6, NULL, &needed, NULL, 0) < 0) {
        snprintf(out, outlen, "sysctl NET_RT_DUMP: %s\n", strerror(errno));
        return;
    }

    char *buf = malloc(needed);
    if (!buf) {
        snprintf(out, outlen, "out of memory\n");
        return;
    }

    if (sysctl(mib, 6, buf, &needed, NULL, 0) < 0) {
        snprintf(out, outlen, "sysctl NET_RT_DUMP(2): %s\n", strerror(errno));
        free(buf);
        return;
    }

    size_t pos = 0;
    pos += snprintf(out + pos, outlen - pos,
        "%-20s %-18s %-6s\n",
        "Destination", "Gateway", "Flags");

    char *lim = buf + needed;
    for (char *next = buf; next < lim; ) {
        struct rt_msghdr *rtm = (struct rt_msghdr *)next;
        next += rtm->rtm_msglen;

        if (rtm->rtm_version != RTM_VERSION)
            continue;
        if (!(rtm->rtm_addrs & RTA_DST))
            continue;

        /* Walk the sockaddr array that follows the header */
        struct sockaddr *sa      = (struct sockaddr *)(rtm + 1);
        struct sockaddr *dst_sa  = NULL;
        struct sockaddr *gw_sa   = NULL;
        struct sockaddr *mask_sa = NULL;

        for (int bit = 0; bit < RTAX_MAX; bit++) {
            if (!(rtm->rtm_addrs & (1 << bit)))
                continue;
            switch (bit) {
            case RTAX_DST:     dst_sa  = sa; break;
            case RTAX_GATEWAY: gw_sa   = sa; break;
            case RTAX_NETMASK: mask_sa = sa; break;
            }
            sa = (struct sockaddr *)((char *)sa + RT_SA_RLEN(sa));
        }

        if (!dst_sa || dst_sa->sa_family != AF_INET)
            continue;

        struct sockaddr_in *dst  = (struct sockaddr_in *)dst_sa;
        struct sockaddr_in *mask =
            mask_sa ? (struct sockaddr_in *)mask_sa : NULL;

        /* Destination string */
        char dst_buf[32];
        if (dst->sin_addr.s_addr == INADDR_ANY &&
            rtm->rtm_flags & RTF_GATEWAY) {
            snprintf(dst_buf, sizeof(dst_buf), "default");
        } else {
            int prefix = 0;
            char addr_s[INET_ADDRSTRLEN];
            if (mask) {
                uint32_t m = ntohl(mask->sin_addr.s_addr);
                while (m & 0x80000000u) { prefix++; m <<= 1; }
            }
            inet_ntop(AF_INET, &dst->sin_addr, addr_s, sizeof(addr_s));
            snprintf(dst_buf, sizeof(dst_buf), "%s/%d", addr_s, prefix);
        }

        /* Gateway string */
        char gw_buf[32] = "-";
        if (gw_sa) {
            if (gw_sa->sa_family == AF_INET) {
                struct sockaddr_in *gw = (struct sockaddr_in *)gw_sa;
                inet_ntop(AF_INET, &gw->sin_addr,
                          gw_buf, (socklen_t)sizeof(gw_buf));
                gw_buf[sizeof(gw_buf) - 1] = '\0';
            } else if (gw_sa->sa_family == AF_LINK) {
                struct sockaddr_dl *sdl = (struct sockaddr_dl *)gw_sa;
                if (sdl->sdl_nlen > 0) {
                    int nlen = sdl->sdl_nlen < (int)sizeof(gw_buf) - 1
                               ? sdl->sdl_nlen : (int)sizeof(gw_buf) - 1;
                    memcpy(gw_buf, sdl->sdl_data, nlen);
                    gw_buf[nlen] = '\0';
                } else {
                    snprintf(gw_buf, sizeof(gw_buf), "link#%d",
                             rtm->rtm_index);
                }
            }
        }

        /* Flags string: build with bounds-checked character appends */
        char flags[8] = "";
        size_t flen = 0;
        if (rtm->rtm_flags & RTF_UP      && flen < sizeof(flags)-1) flags[flen++] = 'U';
        if (rtm->rtm_flags & RTF_GATEWAY && flen < sizeof(flags)-1) flags[flen++] = 'G';
        if (rtm->rtm_flags & RTF_HOST    && flen < sizeof(flags)-1) flags[flen++] = 'H';
        if (rtm->rtm_flags & RTF_STATIC  && flen < sizeof(flags)-1) flags[flen++] = 'S';
        flags[flen] = '\0';

        pos += snprintf(out + pos, outlen - pos,
            "%-20s %-18s %s\n", dst_buf, gw_buf, flags);

        if (pos >= outlen - 128) /* safety margin: stop when nearly full */
            break;
    }

    free(buf);
}

/* -----------------------------------------------------------------------
 * route add <dest> <prefix_len> <gw>
 * Broadcasts RTM_ADD to every worker process.
 * ---------------------------------------------------------------------- */
static int
cmd_route_add(const char *dest, int prefix, const char *gw,
              char *errbuf, size_t errsz)
{
    struct {
        struct rt_msghdr  hdr;
        struct sockaddr_in dst;
        struct sockaddr_in gw_addr;
        struct sockaddr_in mask;
    } msg;

    memset(&msg, 0, sizeof(msg));

    msg.hdr.rtm_msglen  = sizeof(msg);
    msg.hdr.rtm_version = RTM_VERSION;
    msg.hdr.rtm_type    = RTM_ADD;
    msg.hdr.rtm_flags   = RTF_UP | RTF_GATEWAY | RTF_STATIC;
    msg.hdr.rtm_addrs   = RTA_DST | RTA_GATEWAY | RTA_NETMASK;
    msg.hdr.rtm_seq     = (int)getpid();

    msg.dst.sin_family = AF_INET;
    msg.dst.sin_len    = sizeof(struct sockaddr_in);
    if (!inet_aton(dest, &msg.dst.sin_addr)) {
        snprintf(errbuf, errsz, "invalid dest: %s", dest);
        return -1;
    }

    msg.gw_addr.sin_family = AF_INET;
    msg.gw_addr.sin_len    = sizeof(struct sockaddr_in);
    if (!inet_aton(gw, &msg.gw_addr.sin_addr)) {
        snprintf(errbuf, errsz, "invalid gateway: %s", gw);
        return -1;
    }

    msg.mask.sin_family = AF_INET;
    msg.mask.sin_len    = sizeof(struct sockaddr_in);
    /*
     * Build the subnet mask from the prefix length.
     * prefix 0  -> default route: mask stays 0.0.0.0 (skipped below).
     * prefix 1-32: the shift `1u << (32 - prefix)` has `32 - prefix`
     * in [0, 31], which is well-defined in C.
     */
    if (prefix > 0 && prefix <= 32)
        msg.mask.sin_addr.s_addr =
            htonl(~((1u << (32 - prefix)) - 1));

    return broadcast_rtmsg(&msg, sizeof(msg), errbuf, errsz);
}

/* -----------------------------------------------------------------------
 * route del <dest> <prefix_len>
 * Broadcasts RTM_DELETE to every worker process.
 * ---------------------------------------------------------------------- */
static int
cmd_route_del(const char *dest, int prefix, char *errbuf, size_t errsz)
{
    struct {
        struct rt_msghdr  hdr;
        struct sockaddr_in dst;
        struct sockaddr_in mask;
    } msg;

    memset(&msg, 0, sizeof(msg));

    msg.hdr.rtm_msglen  = sizeof(msg);
    msg.hdr.rtm_version = RTM_VERSION;
    msg.hdr.rtm_type    = RTM_DELETE;
    msg.hdr.rtm_flags   = RTF_UP | RTF_STATIC;
    msg.hdr.rtm_addrs   = RTA_DST | RTA_NETMASK;
    msg.hdr.rtm_seq     = (int)getpid();

    msg.dst.sin_family = AF_INET;
    msg.dst.sin_len    = sizeof(struct sockaddr_in);
    if (!inet_aton(dest, &msg.dst.sin_addr)) {
        snprintf(errbuf, errsz, "invalid dest: %s", dest);
        return -1;
    }

    msg.mask.sin_family = AF_INET;
    msg.mask.sin_len    = sizeof(struct sockaddr_in);
    /* Same shift-range guarantee as cmd_route_add */
    if (prefix > 0 && prefix <= 32)
        msg.mask.sin_addr.s_addr =
            htonl(~((1u << (32 - prefix)) - 1));

    return broadcast_rtmsg(&msg, sizeof(msg), errbuf, errsz);
}

/* -----------------------------------------------------------------------
 * Parse "1.2.3.4/24" into dest and prefix_len.
 * The special token "default" maps to "0.0.0.0/0".
 * ---------------------------------------------------------------------- */
static int
parse_cidr(const char *cidr, char *dest, size_t destsz, int *prefix)
{
    if (strcmp(cidr, "default") == 0) {
        strncpy(dest, "0.0.0.0", destsz);
        *prefix = 0;
        return 0;
    }

    const char *slash = strchr(cidr, '/');
    if (slash) {
        size_t n = (size_t)(slash - cidr);
        if (n >= destsz)
            return -1;
        memcpy(dest, cidr, n);
        dest[n] = '\0';
        char *end;
        long pval = strtol(slash + 1, &end, 10);
        if (end == slash + 1 || *end != '\0' || pval < 0 || pval > 32)
            return -1;
        *prefix = (int)pval;
    } else {
        strncpy(dest, cidr, destsz - 1);
        dest[destsz - 1] = '\0';
        *prefix = 32;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Command dispatcher
 * Parses one command line and writes the text response into resp[respsz].
 * ---------------------------------------------------------------------- */
static void
dispatch(const char *cmd_line, char *resp, size_t respsz)
{
    char  errbuf[256] = "";
    char  line[MAX_CMD_LEN];
    char *argv[16];
    int   argc = 0;
    size_t pos;

    strncpy(line, cmd_line, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    /* Strip trailing whitespace / newlines */
    for (int i = (int)strlen(line) - 1;
         i >= 0 && (line[i] == '\n' || line[i] == '\r' || line[i] == ' ');
         i--)
        line[i] = '\0';

    /* Tokenise */
    char *tok = strtok(line, " \t");
    while (tok && argc < 15) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }

    if (argc == 0) {
        snprintf(resp, respsz, "-ERR empty command\n");
        return;
    }

    /* ----------------------------------------------------------------
     * addr
     * ---------------------------------------------------------------- */
    if (strcmp(argv[0], "addr") == 0 && argc >= 2) {

        if (strcmp(argv[1], "show") == 0) {
            pos = (size_t)snprintf(resp, respsz, "+OK\n");
            cmd_addr_show(argc >= 3 ? argv[2] : NULL,
                          resp + pos, respsz - pos - 2);
            return;
        }

        if (strcmp(argv[1], "add") == 0 && argc >= 5) {
            /* addr add <ifname> <ip> <netmask> */
            if (cmd_addr_add(argv[2], argv[3], argv[4],
                             errbuf, sizeof(errbuf)) < 0) {
                snprintf(resp, respsz, "-ERR %s\n", errbuf);
            } else {
                snprintf(resp, respsz,
                    "+OK addr %s added to %s (all %d procs)\n",
                    argv[3], argv[2], nb_procs);
            }
            return;
        }

        if (strcmp(argv[1], "del") == 0 && argc >= 4) {
            /* addr del <ifname> <ip> */
            if (cmd_addr_del(argv[2], argv[3],
                             errbuf, sizeof(errbuf)) < 0) {
                snprintf(resp, respsz, "-ERR %s\n", errbuf);
            } else {
                snprintf(resp, respsz,
                    "+OK addr %s deleted from %s (all %d procs)\n",
                    argv[3], argv[2], nb_procs);
            }
            return;
        }

        snprintf(resp, respsz,
            "-ERR usage: addr show|add|del ...\n");
        return;
    }

    /* ----------------------------------------------------------------
     * link
     * ---------------------------------------------------------------- */
    if (strcmp(argv[0], "link") == 0 && argc >= 2) {

        if (strcmp(argv[1], "show") == 0) {
            pos = (size_t)snprintf(resp, respsz, "+OK\n");
            cmd_addr_show(argc >= 3 ? argv[2] : NULL,
                          resp + pos, respsz - pos - 2);
            return;
        }

        if (strcmp(argv[1], "set") == 0 && argc >= 4) {
            /* link set <ifname> up|down */
            int up = (strcmp(argv[3], "up") == 0) ? 1 : 0;
            if (cmd_link_set(argv[2], up, errbuf, sizeof(errbuf)) < 0) {
                snprintf(resp, respsz, "-ERR %s\n", errbuf);
            } else {
                snprintf(resp, respsz,
                    "+OK %s is now %s (all %d procs)\n",
                    argv[2], up ? "up" : "down", nb_procs);
            }
            return;
        }

        snprintf(resp, respsz,
            "-ERR usage: link show [ifname] | link set <ifname> up|down\n");
        return;
    }

    /* ----------------------------------------------------------------
     * route
     * ---------------------------------------------------------------- */
    if (strcmp(argv[0], "route") == 0 && argc >= 2) {

        if (strcmp(argv[1], "show") == 0) {
            pos = (size_t)snprintf(resp, respsz, "+OK\n");
            cmd_route_show(resp + pos, respsz - pos - 2);
            return;
        }

        if (strcmp(argv[1], "add") == 0 && argc >= 4) {
            /*
             * route add <dest/prefix>  gw <gw>   (5 tokens)
             * route add <dest/prefix> <gw>        (4 tokens)
             */
            char dest[64];
            int  prefix;
            if (parse_cidr(argv[2], dest, sizeof(dest), &prefix) < 0) {
                snprintf(resp, respsz,
                    "-ERR invalid CIDR: %s\n", argv[2]);
                return;
            }
            const char *gw_str = (argc >= 5 &&
                                  strcmp(argv[3], "gw") == 0)
                                 ? argv[4] : argv[3];
            if (cmd_route_add(dest, prefix, gw_str,
                              errbuf, sizeof(errbuf)) < 0) {
                snprintf(resp, respsz, "-ERR %s\n", errbuf);
            } else {
                snprintf(resp, respsz,
                    "+OK route %s/%d via %s added (all %d procs)\n",
                    dest, prefix, gw_str, nb_procs);
            }
            return;
        }

        if (strcmp(argv[1], "del") == 0 && argc >= 3) {
            /* route del <dest/prefix> */
            char dest[64];
            int  prefix;
            if (parse_cidr(argv[2], dest, sizeof(dest), &prefix) < 0) {
                snprintf(resp, respsz,
                    "-ERR invalid CIDR: %s\n", argv[2]);
                return;
            }
            if (cmd_route_del(dest, prefix,
                              errbuf, sizeof(errbuf)) < 0) {
                snprintf(resp, respsz, "-ERR %s\n", errbuf);
            } else {
                snprintf(resp, respsz,
                    "+OK route %s/%d deleted (all %d procs)\n",
                    dest, prefix, nb_procs);
            }
            return;
        }

        snprintf(resp, respsz,
            "-ERR usage: route show|add|del ...\n");
        return;
    }

    /* ----------------------------------------------------------------
     * status
     * ---------------------------------------------------------------- */
    if (strcmp(argv[0], "status") == 0) {
        snprintf(resp, respsz,
            "+OK ff_netd running, nb_procs=%d, socket=%s\n",
            nb_procs, socket_path);
        return;
    }

    snprintf(resp, respsz,
        "-ERR unknown command '%s'\n"
        "Supported commands:\n"
        "  addr show [<ifname>]\n"
        "  addr add  <ifname> <ip> <netmask>\n"
        "  addr del  <ifname> <ip>\n"
        "  link show [<ifname>]\n"
        "  link set  <ifname> up|down\n"
        "  route show\n"
        "  route add <dest/prefix> [gw] <gw>\n"
        "  route del <dest/prefix>\n"
        "  status\n",
        argv[0]);
}

/* -----------------------------------------------------------------------
 * Handle a single client connection.
 * Read one command line, dispatch it, write the response, close.
 * ---------------------------------------------------------------------- */
static void
handle_client(int fd)
{
    char cmd[MAX_CMD_LEN];
    char *resp = NULL;

    ssize_t n = recv(fd, cmd, sizeof(cmd) - 1, 0);
    if (n <= 0)
        goto done;
    cmd[n] = '\0';

    /* Terminate at first newline so extra data is ignored */
    char *nl = strchr(cmd, '\n');
    if (nl)
        *nl = '\0';

    resp = malloc(MAX_RESP_BUF);
    if (!resp) {
        const char *oom = "-ERR out of memory\n";
        send(fd, oom, strlen(oom), MSG_NOSIGNAL);
        goto done;
    }

    resp[0] = '\0';
    dispatch(cmd, resp, MAX_RESP_BUF);

    size_t len = strlen(resp);
    send(fd, resp, len, MSG_NOSIGNAL);

done:
    free(resp);
    close(fd);
}

/* -----------------------------------------------------------------------
 * Initialise the Unix-domain socket server.
 * ---------------------------------------------------------------------- */
static int
server_init(const char *path)
{
    struct unix_addr addr;

    /* Remove stale socket file from a previous run */
    if (unlink(path) < 0 && errno != ENOENT)
        fprintf(stderr, "ff_netd: warning: unlink(%s): %s\n",
                path, strerror(errno));

    /*
     * socket() resolves to the real Linux syscall via glibc; AF_UNIX=1
     * is identical on both Linux and FreeBSD.
     */
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, "ff_netd: socket(AF_UNIX): %s\n", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sa_family = AF_UNIX;   /* 1 – matches Linux sa_family layout */
    strncpy(addr.sa_path, path, sizeof(addr.sa_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "ff_netd: bind(%s): %s\n", path, strerror(errno));
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 8) < 0) {
        fprintf(stderr, "ff_netd: listen: %s\n", strerror(errno));
        close(server_fd);
        return -1;
    }

    fprintf(stdout, "ff_netd: listening on %s\n", path);
    return 0;
}

/* -----------------------------------------------------------------------
 * Main accept loop – single-threaded, one request per connection.
 * ---------------------------------------------------------------------- */
static void
server_loop(void)
{
    for (;;) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "ff_netd: accept: %s\n", strerror(errno));
            break;
        }
        handle_client(client);
    }
}

/* -----------------------------------------------------------------------
 * Signal handler – clean up and exit.
 * ---------------------------------------------------------------------- */
static void
sig_handler(int sig)
{
    (void)sig;
    if (server_fd >= 0)
        close(server_fd);
    unlink(socket_path);
    ff_ipc_exit();
    _exit(0);
}

/* -----------------------------------------------------------------------
 * Usage
 * ---------------------------------------------------------------------- */
static void
usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-s socket_path]\n"
        "\n"
        "Options:\n"
        "  -s path   Path of the Unix domain socket (default: %s)\n"
        "\n"
        "ff_netd must be started AFTER the F-Stack worker processes.\n",
        prog, FF_NETD_SOCKET_DEFAULT);
}

/* -----------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int
main(int argc, char *argv[])
{
    int opt;
    strncpy(socket_path, FF_NETD_SOCKET_DEFAULT, sizeof(socket_path) - 1);

    while ((opt = getopt(argc, argv, "s:h")) != -1) {
        switch (opt) {
        case 's':
            strncpy(socket_path, optarg, sizeof(socket_path) - 1);
            break;
        case 'h':
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* -----------------------------------------------------------------
     * Initialise as a DPDK secondary process and connect to the shared
     * memory / ring infrastructure created by the primary workers.
     * ----------------------------------------------------------------- */
    fprintf(stdout, "ff_netd: initialising DPDK secondary process ...\n");
    if (ff_ipc_init() != 0) {
        fprintf(stderr, "ff_netd: ff_ipc_init failed\n");
        return 1;
    }

    /* Auto-discover how many worker processes are running */
    nb_procs = discover_nb_procs();
    if (nb_procs == 0) {
        fprintf(stderr,
            "ff_netd: no F-Stack worker processes found.\n"
            "         Start the workers first, then run ff_netd.\n");
        ff_ipc_exit();
        return 1;
    }
    fprintf(stdout, "ff_netd: found %d worker process(es)\n", nb_procs);

    /* Clean up on SIGINT / SIGTERM, ignore SIGPIPE */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Start the Unix-socket server */
    if (server_init(socket_path) < 0) {
        ff_ipc_exit();
        return 1;
    }

    server_loop();

    unlink(socket_path);
    ff_ipc_exit();
    return 0;
}
