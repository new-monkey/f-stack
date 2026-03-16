#include <arpa/inet.h>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include "ReactorServer.h"

namespace {

volatile sig_atomic_t g_stop = 0;

void onSignal(int) {
    g_stop = 1;
}

std::string encodeFrame(uint32_t code, std::string_view payload) {
    std::string frame;
    frame.resize(8 + payload.size());

    const uint32_t netLen = htonl(static_cast<uint32_t>(payload.size()));
    const uint32_t netCode = htonl(code);
    std::memcpy(&frame[0], &netLen, sizeof(netLen));
    std::memcpy(&frame[4], &netCode, sizeof(netCode));
    if (!payload.empty()) {
        std::memcpy(&frame[8], payload.data(), payload.size());
    }
    return frame;
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 19090;
    if (argc >= 2) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    EventLoop::Options options;
    options.maxPayloadBytes = 40960;
    options.maxOutputBufferBytes = 1U << 20;
    options.overflowPolicy = TcpConnection::OverflowPolicy::kDropNewData;

    ReactorServer server(EpollBackendType::kKernel, options);
    if (!server.init("0.0.0.0", port, 1024)) {
        std::perror("ReactorServer::init");
        return 1;
    }

    server.dispatcher().registerHandler(1, [](uint32_t code, std::string_view payload, const std::shared_ptr<TcpConnection>& conn) {
        const std::string frame = encodeFrame(code, payload);
        (void)conn->send(frame);
    });

    server.dispatcher().setDefaultHandler([](uint32_t code, std::string_view, const std::shared_ptr<TcpConnection>&) {
        std::cerr << "unknown MsgCode: " << code << std::endl;
    });

    std::cout << "dev kernel echo server listening on 0.0.0.0:" << port << std::endl;

    while (!g_stop) {
        server.serveOnce(50);
    }

    const auto& stats = server.stats();
    std::cout << "server stats: accepted=" << stats.totalAccepted
              << ", closed=" << stats.totalClosed
              << ", active=" << stats.activeConnections
              << ", bytes_read=" << stats.totalBytesRead
              << ", bytes_written=" << stats.totalBytesWritten
              << ", frames=" << stats.totalFramesDispatched
              << ", dropped=" << stats.totalDroppedBytes
              << ", protocol_errors=" << stats.totalProtocolErrors
              << std::endl;

    return 0;
}
