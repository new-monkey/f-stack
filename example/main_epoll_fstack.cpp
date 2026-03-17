#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_set>

#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "../app/epoll/Acceptor.h"
#include "../app/epoll/TcpConnection.h"

#include "ff_config.h"
#include "ff_api.h"
#include "ff_epoll.h"
#include "ff_log.h"


namespace {

constexpr std::size_t kMaxEvents = 512;
volatile sig_atomic_t g_stop = 0;

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
    std::exit(1);
}

bool writeAllNonBlocking(int fd, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ff_write(fd, data + sent, len - sent);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return false;
        }

        return false;
    }

    return true;
}

void closeClient(Epoller& epoller, std::unordered_set<int>& clients, int fd) {
    (void)epoller.del(fd);
    clients.erase(fd);
    (void)::close(fd);
    std::cout << "closed client fd " << fd << std::endl;
}


void runOnce(Epoller& epoller, Acceptor& acceptor, std::unordered_set<int>& clients) {
    const int nevents = epoller.wait(10);
        if (nevents < 0) {
            if (errno == EINTR) {
                return;

            }
            std::perror("epoller.wait");
            return;
        }

        for (int i = 0; i < nevents; ++i) {
            const struct epoll_event& ev = epoller.eventAt(static_cast<std::size_t>(i));
            const int fd = ev.data.fd;

            if (fd == acceptor.fd()) {
                acceptor.acceptReady();
                return;
            }

            if (clients.find(fd) == clients.end()) {
                std::cerr << "unknown fd " << fd << " in epoll events" << std::endl;
                return;
            }

            if (ev.events & static_cast<uint32_t>(EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                std::cerr << "error event on fd " << fd << ", closing connection" << std::endl;
                closeClient(epoller, clients, fd);
                return;
            }

            if (ev.events & static_cast<uint32_t>(EPOLLIN)) {
                char buf[1024];
                const ssize_t n = ff_read(fd, buf, sizeof(buf));
                if (n > 0) {
                    if (!writeAllNonBlocking(fd, kHtml, sizeof(kHtml) - 1)) {
                        std::cerr << "write to fd " << fd << " would block, closing connection" << std::endl;
                        closeClient(epoller, clients, fd);
                    }
                    return;
                }

                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                    std::cerr << "read error on fd " << fd << ", closing connection" << std::endl;
                    closeClient(epoller, clients, fd);
                }
            }
        }
 }

 struct RunContext {
     Epoller& epoller;
     Acceptor& acceptor;
     std::unordered_set<int>& clients;
 };

int loop(void *arg) {
    auto* ctx = static_cast<RunContext*>(arg);
    runOnce(ctx->epoller, ctx->acceptor, ctx->clients);
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

    Epoller epoller(EpollBackendType::kFStack, kMaxEvents);
    if (!epoller.open()) {
        std::perror("epoller.open");
        return 1;
    }

    Acceptor acceptor(EpollBackendType::kFStack);
    if (!acceptor.open("0.0.0.0", port, static_cast<int>(kMaxEvents))) {
        std::perror("acceptor.open");
        return 1;
    }

    std::unordered_set<int> clients;
    acceptor.setNewConnectionCallback([&](int clientFd) {
        const uint32_t ev = static_cast<uint32_t>(EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP);
        if (!epoller.add(clientFd, ev)) {
            (void)::close(clientFd);
            return;
        }
        clients.insert(clientFd);

    });

    if (!epoller.add(acceptor.fd(), static_cast<uint32_t>(EPOLLIN))) {
        std::perror("epoller.add(listenFd)");
        return 1;
    }

    std::cout << "kernel epoll http server listening on 0.0.0.0:" << port << std::endl;

    RunContext ctx{epoller, acceptor, clients};
    ff_run(loop, &ctx);

    for (int fd : clients) {
        (void)epoller.del(fd);
        (void)::close(fd);
    }

    return 0;
}
