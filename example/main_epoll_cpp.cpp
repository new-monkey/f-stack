#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_set>

#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "../app/epoll/Acceptor.h"

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
}

bool writeAllNonBlocking(int fd, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::write(fd, data + sent, len - sent);
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
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 8080;
    if (argc >= 2) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Epoller epoller(EpollBackendType::kKernel, kMaxEvents);
    if (!epoller.open()) {
        std::perror("epoller.open");
        return 1;
    }

    Acceptor acceptor(EpollBackendType::kKernel);
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

    while (!g_stop) {
        const int nevents = epoller.wait(1000);
        if (nevents < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("epoller.wait");
            break;
        }

        for (int i = 0; i < nevents; ++i) {
            const struct epoll_event& ev = epoller.eventAt(static_cast<std::size_t>(i));
            const int fd = ev.data.fd;

            if (fd == acceptor.fd()) {
                acceptor.acceptReady();
                continue;
            }

            if (clients.find(fd) == clients.end()) {
                continue;
            }

            if (ev.events & static_cast<uint32_t>(EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                closeClient(epoller, clients, fd);
                continue;
            }

            if (ev.events & static_cast<uint32_t>(EPOLLIN)) {
                char buf[1024];
                const ssize_t n = ::read(fd, buf, sizeof(buf));
                if (n > 0) {
                    if (!writeAllNonBlocking(fd, kHtml, sizeof(kHtml) - 1)) {
                        closeClient(epoller, clients, fd);
                    }
                    continue;
                }

                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                    closeClient(epoller, clients, fd);
                }
            }
        }
    }

    for (int fd : clients) {
        (void)epoller.del(fd);
        (void)::close(fd);
    }

    return 0;
}
