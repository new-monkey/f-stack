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

void testDispatcherPayloadIncludesMsgCode() {
    int fds[2];
    const int rc = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    assert(rc == 0);

    MessageDispatcher dispatcher;
    bool called = false;
    uint32_t seenCode = 0;
    std::string seenPayload;
    dispatcher.registerHandler(1001, [&](uint32_t code, std::string_view payload, const std::shared_ptr<TcpConnection>&) {
        called = true;
        seenCode = code;
        seenPayload.assign(payload.data(), payload.size());
    });

    TcpConnection::Options options;
    options.maxPayloadBytes = 40960;

    auto conn = std::make_shared<TcpConnection>(fds[0], makeKernelIoOps(), &dispatcher, options);
    conn->markConnected();

    const uint32_t msgCode = 1001;
    const std::string body = "hello";
    const uint32_t payloadLenHost = static_cast<uint32_t>(sizeof(uint32_t) + body.size());
    const uint32_t netLen = htonl(payloadLenHost);
    const uint32_t netCode = htonl(msgCode);

    std::string frame;
    frame.resize(sizeof(netLen) + sizeof(netCode) + body.size());
    std::memcpy(frame.data(), &netLen, sizeof(netLen));
    std::memcpy(frame.data() + sizeof(netLen), &netCode, sizeof(netCode));
    std::memcpy(frame.data() + sizeof(netLen) + sizeof(netCode), body.data(), body.size());

    const ssize_t wn = ::write(fds[1], frame.data(), frame.size());
    assert(wn == static_cast<ssize_t>(frame.size()));
    ::close(fds[1]);

    conn->handleReadEvent();

    assert(called);
    assert(seenCode == msgCode);
    assert(seenPayload.size() == payloadLenHost);

    uint32_t payloadNetCode = 0;
    std::memcpy(&payloadNetCode, seenPayload.data(), sizeof(payloadNetCode));
    assert(payloadNetCode == netCode);

    const std::string payloadBody = seenPayload.substr(sizeof(uint32_t));
    assert(payloadBody == body);
    assert(conn->framesDispatched() == 1);
}

}  // namespace

int main() {
    testPayloadCap();
    testOverflowDropNew();
    testDispatcherPayloadIncludesMsgCode();
    std::cout << "dev_policy_test passed" << std::endl;
    return 0;
}
