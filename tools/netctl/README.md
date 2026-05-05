# ff_netd / ff_netctl – F-Stack Network Configuration Agent

## Overview

This module implements a lightweight network-configuration management
layer for F-Stack multi-process deployments.

```
┌─────────────┐    Unix socket      ┌────────────────────────────┐
│ ff_netctl   │ ──────────────────▶ │          ff_netd            │
│  (CLI)      │ ◀─────────────────  │  (Config Agent Daemon)      │
└─────────────┘   text responses    │                             │
                                    │  DPDK secondary process     │
                                    └──────────────┬──────────────┘
                                                   │ DPDK rings (shared hugepages)
                   ┌───────────────────────────────┼───────────────────────┐
                   │                               │                       │
          ┌────────▼────────┐           ┌──────────▼──────┐    ┌──────────▼──────┐
          │  Worker proc 0  │           │  Worker proc 1  │    │  Worker proc N  │
          │  (PRIMARY DPDK) │           │  (PRIMARY DPDK) │    │  (PRIMARY DPDK) │
          │  FreeBSD stack  │           │  FreeBSD stack  │    │  FreeBSD stack  │
          └─────────────────┘           └─────────────────┘    └─────────────────┘
```

### Process Model

**ff_netd** runs as a DPDK *secondary* process alongside the primary
F-Stack worker processes.  It:

1. Attaches to the same DPDK shared-memory region as the workers.
2. Auto-discovers the number of running workers by probing the
   `ff_msg_ring_in_N` rings (the same rings that `ff_ifconfig`,
   `ff_route`, etc. use).
3. Listens on a Unix-domain socket for incoming CLI connections.
4. For each received command:
   - **Read commands** (addr/route show): query proc 0 only – all
     workers share the same protocol-stack state.
   - **Write commands** (add/del addr, link set, route add/del):
     broadcast the IPC message to **all** worker processes so every
     FreeBSD network-stack instance is updated consistently.

No modifications to the worker processes are required.

---

## Building

```
cd tools/netctl
make TOPDIR=../..
```

Requires DPDK to be installed (same pre-requisite as all other
F-Stack tools).  Binaries are placed in `tools/sbin/`.

---

## Usage

### Start the daemon

```
# Must be run AFTER the F-Stack worker processes are up
ff_netd [-s /var/run/ff_netd.sock]
```

### CLI client

```
ff_netctl [-s socket] <command> [args ...]
```

#### Supported commands

| Command | Description |
|---------|-------------|
| `addr show [<ifname>]` | List interface addresses |
| `addr add <ifname> <ip> <netmask>` | Add an IPv4 address |
| `addr del <ifname> <ip>` | Remove an IPv4 address |
| `link show [<ifname>]` | Show link status |
| `link set <ifname> up\|down` | Bring an interface up or down |
| `route show` | Show IPv4 routing table |
| `route add <dest/prefix> [gw] <gateway>` | Add a static route |
| `route del <dest/prefix>` | Delete a route |
| `status` | Show daemon status |

#### Examples

```
# Show all interfaces
ff_netctl addr show

# Add an IP address to f-stack0
ff_netctl addr add f-stack0 192.168.1.10 255.255.255.0

# Remove an address
ff_netctl addr del f-stack0 192.168.1.10

# Bring an interface down
ff_netctl link set f-stack0 down

# Show routing table
ff_netctl route show

# Add default route
ff_netctl route add default gw 192.168.1.1

# Add a subnet route
ff_netctl route add 10.0.0.0/24 gw 192.168.1.254

# Delete a route
ff_netctl route del 10.0.0.0/24

# Query daemon status
ff_netctl status
```

---

## Design Notes

### Why a daemon?

The existing tools (`ff_ifconfig`, `ff_route`, etc.) are short-lived:
they perform one operation against a single worker process (selected
via `-p <proc_id>`) and exit.  A user who wants to configure N
workers must run each tool N times.

**ff_netd** provides a single entry point that automatically
broadcasts write operations to every worker, hiding the multi-process
complexity from the operator.

### IPC mechanism

ff_netd reuses the same `libffcompat` IPC layer (`ff_ipc_send` /
`ff_ipc_recv`, `ioctl_va`, `rtioctl`, `sysctl`) that the existing
tools use.  The key addition is `ff_set_proc_id(i)` called before
each IPC operation so the message is directed to worker *i*.

### Socket protocol

The protocol is intentionally simple (line-oriented text) so that
`nc` or `socat` can be used for scripting and debugging:

```
echo "addr show" | socat - UNIX-CONNECT:/var/run/ff_netd.sock
```

Response format:
- `+OK\n<content>` – success, content is zero or more text lines.
- `-ERR <message>\n` – failure.
