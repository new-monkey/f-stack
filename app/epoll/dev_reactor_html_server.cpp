#include <arpa/inet.h>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include "FrameCodec.h"
#include "ReactorServer.h"

namespace {

volatile sig_atomic_t g_stop = 0;

void onSignal(int) {
    g_stop = 1;
}

using BusinessDispatch = std::function<std::string(uint32_t code, std::string_view requestBody)>;

void installDispatcher(ReactorServer& server, BusinessDispatch dispatch) {
    server.dispatcher().registerHandler(1, [dispatch](uint32_t code, std::string_view payload, const std::shared_ptr<TcpConnection>& conn) {
        uint32_t payloadCode = 0;
        if (!FrameCodec::extractMsgCode(payload, payloadCode)) {
            return;
        }
        if (payloadCode != code) {
            return;
        }

        const std::string_view requestBody = FrameCodec::bodyFromPayload(payload);
        const std::string responseBody = dispatch(code, requestBody);
        const std::string frame = FrameCodec::encode(code, responseBody);
        (void)conn->send(frame);
    });

    server.dispatcher().setDefaultHandler([](uint32_t code, std::string_view, const std::shared_ptr<TcpConnection>& conn) {
        static const std::string kNotFoundHtml =
            "<!DOCTYPE html><html><head><title>Not Found</title></head>"
            "<body><h1>404</h1><p>Unknown message code.</p></body></html>";
        const std::string frame = FrameCodec::encode(code, kNotFoundHtml);
        (void)conn->send(frame);
    });
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 19091;
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

    const BusinessDispatch htmlDispatch = [](uint32_t, std::string_view requestBody) {
        std::string body;
        body.reserve(256 + requestBody.size());
        body += "<!DOCTYPE html><html><head><title>Reactor HTML</title></head><body>";
        body += "<h1>Hello From ReactorServer</h1>";
        body += "<p>Business logic is injected by callback.</p>";
        body += "<pre>Request: ";
        body.append(requestBody.data(), requestBody.size());
        body += "</pre></body></html>";
        return body;
    };

    installDispatcher(server, htmlDispatch);

    std::cout << "reactor html server (kernel epoll) listening on 0.0.0.0:" << port << std::endl;

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
