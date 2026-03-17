#include <csignal>
#include <iostream>
#include <memory>
#include <string_view>

#include <errno.h>

#include "../app/epoll/Acceptor.h"
#include "../app/epoll/EventLoop.h"
#include "../app/epoll/TcpConnection.h"

#include "../lib/ff_api.h"


namespace {

constexpr std::size_t kMaxEvents = 512;
volatile sig_atomic_t g_stop = 0;
constexpr int kLoopTimeoutMs = 10;

const char kHtml[] =
    "HTTP/1.1 200 OK\r\n"
    "Server: Cpp-Epoll\r\n"
    "Content-Type: text/html\r\n"
    "Content-Length: 104\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "<!DOCTYPE html><html><head><title>Welcome</title></head>"
    "<body><h1>Welcome to C++ Epoll Server!</h1></body></html>";

void onSignal(int) {
    g_stop = 1;
}

void installHttpHandler(const std::shared_ptr<TcpConnection>& conn) {
    static constexpr std::string_view kResponse(kHtml, sizeof(kHtml) - 1);
    conn->setRawMessageCallback([](std::string_view, const std::shared_ptr<TcpConnection>& connection) {
        if (!connection->send(kResponse)) {
            std::cerr << "send failed on fd " << connection->fd() << ", closing connection" << std::endl;
            connection->shutdown();
        }
    });
}

 struct RunContext {
     EventLoop& loop;
     Acceptor& acceptor;
 };

int loop(void *arg) {
    auto* ctx = static_cast<RunContext*>(arg);

    if (g_stop != 0) {
        ff_stop_run();
        return 0;
    }

    ctx->acceptor.acceptReady();

    const int nevents = ctx->loop.loopOnce(kLoopTimeoutMs);
    if (nevents < 0 && errno != EINTR) {
        std::perror("EventLoop::loopOnce");
        ff_stop_run();
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv) {

    ff_init(argc, argv);

    uint16_t port = 8080;
    // if (argc >= 2) {
    //     port = static_cast<uint16_t>(std::stoi(argv[1]));
    // }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    EventLoop::Options options;
    options.maxEvents = kMaxEvents;
    options.enableFrameCodec = false;
    options.overflowPolicy = TcpConnection::OverflowPolicy::kDropNewData;

    EventLoop eventLoop(EpollBackendType::kFStack, options);
    if (!eventLoop.init()) {
        std::perror("eventLoop.init");
        return 1;
    }

    Acceptor acceptor(EpollBackendType::kFStack);
    if (!acceptor.open("0.0.0.0", port, static_cast<int>(kMaxEvents))) {
        std::perror("acceptor.open");
        return 1;
    }

    acceptor.setNewConnectionCallback([&](int clientFd) {
        if (!eventLoop.addConnection(clientFd, installHttpHandler)) {
            (void)ff_close(clientFd);
            return;
        }
    });

    std::cout << "f-stack event loop http server listening on 0.0.0.0:" << port << std::endl;

    RunContext ctx{eventLoop, acceptor};
    ff_run(loop, &ctx);

    const auto& stats = eventLoop.stats();
    std::cout << "event loop stats: accepted=" << stats.totalAccepted
              << ", closed=" << stats.totalClosed
              << ", active=" << stats.activeConnections
              << ", bytes_read=" << stats.totalBytesRead
              << ", bytes_written=" << stats.totalBytesWritten
              << std::endl;

    return 0;
}
