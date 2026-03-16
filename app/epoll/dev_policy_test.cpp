#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "MessageDispatcher.h"
#include "TcpConnection.h"

namespace {

TcpConnection::IoOps makeKernelIoOps() {
    TcpConnection::IoOps ops;
    ops.readFn = [](int fd, void* buf, size_t len) { return ::read(fd, buf, len); };
    ops.writeFn = [](int fd, const void* buf, size_t len) { return ::write(fd, buf, len); };
    ops.closeFn = [](int fd) { return ::close(fd); };
    return ops;
}

void testPayloadCap() {
    int fds[2];
    const int rc = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    assert(rc == 0);

    MessageDispatcher dispatcher;
    TcpConnection::Options options;
    options.maxPayloadBytes = 40960;

    auto conn = std::make_shared<TcpConnection>(fds[0], makeKernelIoOps(), &dispatcher, options);
    conn->markConnected();

    const uint32_t netLen = htonl(50000);
    const uint32_t netCode = htonl(1);
    char header[8];
    std::memcpy(header, &netLen, sizeof(netLen));
    std::memcpy(header + 4, &netCode, sizeof(netCode));

    const ssize_t wn = ::write(fds[1], header, sizeof(header));
    assert(wn == static_cast<ssize_t>(sizeof(header)));

    ::close(fds[1]);

    conn->handleReadEvent();
    assert(conn->protocolErrorCount() == 1);
}

void testOverflowDropNew() {
    MessageDispatcher dispatcher;

    TcpConnection::IoOps ops;
    ops.readFn = [](int, void*, size_t) {
        errno = EAGAIN;
        return static_cast<ssize_t>(-1);
    };
    ops.writeFn = [](int, const void*, size_t) {
        errno = EAGAIN;
        return static_cast<ssize_t>(-1);
    };
    ops.closeFn = [](int) { return 0; };

    TcpConnection::Options options;
    options.maxOutputBufferBytes = 16;
    options.overflowPolicy = TcpConnection::OverflowPolicy::kDropNewData;

    auto conn = std::make_shared<TcpConnection>(123, std::move(ops), &dispatcher, options);
    conn->markConnected();

    const std::string data(32, 'x');
    const bool ok = conn->send(data);
    assert(!ok);
    assert(conn->droppedBytes() == 16);
}

}  // namespace

int main() {
    testPayloadCap();
    testOverflowDropNew();
    std::cout << "dev_policy_test passed" << std::endl;
    return 0;
}
