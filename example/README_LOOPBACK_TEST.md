# F-Stack Loopback Interface Test Example

This example demonstrates the loopback interface functionality in F-Stack.

## Overview

The `loopback_test` program shows how to use F-Stack's internal loopback interface (lo0) for communication between threads within the same F-Stack process.

- **Server Thread**: Binds to `127.0.0.1:8080` and listens for connections
- **Client Thread**: Connects to `127.0.0.1:8080` and sends a test message
- **Communication**: All traffic goes through F-Stack's lo0 interface (not the kernel's loopback)

## Prerequisites

1. F-Stack must be built and installed:
   ```bash
   cd /path/to/f-stack/lib
   make && make install
   ```

2. DPDK must be properly configured (hugepages, NIC binding, etc.)

## Building

```bash
cd /path/to/f-stack/example
make
```

This will compile `loopback_test` along with other example programs.

## Configuration

Create or modify `config.ini` to match your system configuration. A minimal configuration example:

```ini
[dpdk]
lcore_mask=1
channel=4
promiscuous=1
numa_on=0

# You still need at least one physical port configured
port_list=0

[port0]
addr=192.168.1.2
netmask=255.255.255.0
gateway=192.168.1.1

[freebsd.boot]
hz=100
kern.ipc.maxsockets=262144

[freebsd.sysctl]
kern.ipc.somaxconn=32768
```

**Note**: Even though this test uses loopback, F-Stack still requires at least one DPDK-bound NIC to initialize properly.

## Running

```bash
sudo ./loopback_test -c config.ini
```

Expected output:
```
=================================================
  F-Stack Loopback Interface Test
=================================================

Initializing F-Stack...
F-Stack initialized successfully

[Server] Starting on 127.0.0.1:8080
[Server] Listening on 127.0.0.1:8080 (F-Stack loopback)
[Client] Waiting for server to start...
[Client] Connecting to 127.0.0.1:8080
[Client] Connected successfully (via F-Stack lo0)
[Client] Sending: Hello from F-Stack loopback!
[Server] Accepted connection from 127.0.0.1:xxxxx
[Server] Received 29 bytes: Hello from F-Stack loopback!
[Server] ✓ Message verification passed!
[Client] Sent 29 bytes
[Client] Received response: ACK from server
[Client] Closed
[Server] Closed

=================================================
  TEST RESULT: ✓ PASSED
  F-Stack loopback interface is working!
=================================================
```

## What This Test Demonstrates

✅ **F-Stack to F-Stack communication via loopback works**
- Both server and client use F-Stack APIs (`ff_socket`, `ff_bind`, `ff_connect`, etc.)
- Communication happens through F-Stack's internal lo0 interface (127.0.0.1)
- No kernel involvement in the data path

## Important Notes

### ✅ What Works
- F-Stack application to F-Stack application via 127.0.0.1 (same process)
- Multiple threads using loopback within the same F-Stack instance
- All F-Stack socket APIs (`ff_socket`, `ff_connect`, `ff_read`, `ff_write`, etc.)

### ❌ What Doesn't Work
- F-Stack cannot connect to kernel applications on 127.0.0.1
- F-Stack cannot access services using standard `socket()` API via localhost
- Cross-stack communication between F-Stack and kernel requires workarounds

### 🔄 Workaround for Kernel Services
If you need to access kernel services on localhost, use the **dual-stack hybrid approach**:
- Use F-Stack APIs for client-facing traffic (high performance)
- Use kernel `socket()` APIs for localhost backend services

See `main_proxy.c` and `README_PROXY.md` for a complete example.

## Use Cases

This loopback functionality is useful for:

1. **Microservices Architecture**: Co-located services communicating within the same process
2. **Testing**: Testing network code without physical NICs
3. **Development**: Developing and debugging F-Stack applications
4. **IPC**: Inter-thread communication using TCP/UDP within an F-Stack application
5. **Service Mesh**: Sidecar patterns where all services run in the same F-Stack instance

## Performance

Loopback communication in F-Stack offers:
- **Very low latency** (memory copy only, no physical NIC)
- **No packet serialization** overhead
- **No interrupt handling**
- **High throughput** for intra-process communication

Default loopback MTU: 16384 bytes

## Troubleshooting

### Test fails with "ff_connect failed"

**Possible causes:**
1. Server thread hasn't started listening yet (should auto-wait in the code)
2. Port 8080 is already in use by another F-Stack application
3. Loopback interface not initialized (check F-Stack logs)

**Solution:**
- Check F-Stack initialization logs
- Ensure no other F-Stack application is using port 8080
- Verify F-Stack version is v1.21.1 or later (loopback support added)

### "No installation of DPDK found" error

**Solution:**
```bash
export PKG_CONFIG_PATH=/usr/lib64/pkgconfig:/usr/local/lib64/pkgconfig
```

### Segmentation fault on startup

**Possible causes:**
1. Hugepages not configured
2. DPDK NIC not bound
3. Config file has errors

**Solution:**
- Verify DPDK setup (hugepages, NIC binding)
- Check config.ini for syntax errors
- Ensure at least one NIC is properly configured

## Related Documentation

- [F-Stack Loopback FAQ](../doc/F-Stack_Loopback_FAQ.md) - Comprehensive FAQ about loopback support
- [F-Stack API Reference](../doc/F-Stack_API_Reference.md) - Complete API documentation
- [Proxy Example](README_PROXY.md) - Dual-stack hybrid architecture for kernel interop

## Technical Details

### Loopback Initialization

F-Stack automatically initializes the loopback interface during startup:

```c
// From lib/ff_freebsd_init.c
int lo_set_defaultaddr(void) {
    // Sets up lo0 with:
    // - Address: 127.0.0.1
    // - Netmask: 255.0.0.0
    // - MTU: 16384 (default)
}
```

This is called automatically in `ff_init()`, so no manual configuration is needed.

### Architecture

```
┌─────────────────────────────────────────┐
│       loopback_test Process             │
│                                         │
│  ┌─────────────┐    ┌──────────────┐   │
│  │   Server    │    │    Client    │   │
│  │   Thread    │    │    Thread    │   │
│  └──────┬──────┘    └──────┬───────┘   │
│         │                  │            │
│         │  ff_accept()     │            │
│         │  ff_read()       │ ff_connect()
│         │  ff_write()      │ ff_write() │
│         │                  │ ff_read()  │
│         └──────────┬───────┘            │
│                    │                    │
│         ┌──────────▼──────────┐         │
│         │   F-Stack lo0       │         │
│         │   127.0.0.1         │         │
│         └─────────────────────┘         │
│                                         │
│         ┌─────────────────────┐         │
│         │  F-Stack TCP/IP     │         │
│         │  (FreeBSD Stack)    │         │
│         └─────────────────────┘         │
└─────────────────────────────────────────┘
```

All traffic stays within the F-Stack process - no kernel involvement!

## Version Requirements

- **Minimum F-Stack version**: v1.21.1 (released 2021.09)
- **Loopback support added**: v1.21.1 and v1.22 (2021-2022)
- **Current version**: Check `VERSION` file in repository root

To verify your F-Stack version supports loopback:

```bash
# Check release notes
cat /path/to/f-stack/doc/F-Stack_Release_Note.md | grep -A 5 "loopback"

# Or check the VERSION file
cat /path/to/f-stack/VERSION
```

## License

This example is part of F-Stack and follows the same BSD license. See LICENSE file in the repository root.
