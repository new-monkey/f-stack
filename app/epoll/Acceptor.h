#pragma once

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Epoller.h"

#if defined(FSTACK_ENABLE_BACKEND)
#include "../../lib/ff_api.h"
#endif

class Acceptor {
public:
    using NewConnectionCallback = std::function<void(int)>;

    explicit Acceptor(EpollBackendType backendType)
        : backendType_(backendType), listenFd_(-1) {}

    ~Acceptor() { close(); }

    bool open(const std::string& ip, uint16_t port, int backlog = 1024) {
        if (listenFd_ >= 0) {
            return true;
        }

        if (!createListenSocket()) {
            return false;
        }

        setReuseAddr();

        if (!bindAddress(ip, port)) {
            close();
            return false;
        }

        if (!listenInternal(backlog)) {
            close();
            return false;
        }

        if (!setNonBlocking(listenFd_)) {
            close();
            return false;
        }

        return true;
    }

    void close() {
        if (listenFd_ < 0) {
            return;
        }

        if (backendType_ == EpollBackendType::kFStack) {
#if defined(FSTACK_ENABLE_BACKEND)
            (void)ff_close(listenFd_);
#else
            (void)::close(listenFd_);
#endif
        } else {
            (void)::close(listenFd_);
        }

        listenFd_ = -1;
    }

    int fd() const { return listenFd_; }

    void setNewConnectionCallback(NewConnectionCallback cb) { callback_ = std::move(cb); }

    void acceptReady() {
        for (;;) {
            const int clientFd = acceptOne();
            if (clientFd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                break;
            }

            if (!setNonBlocking(clientFd)) {
                closeFd(clientFd);
                continue;
            }

            if (callback_) {
                callback_(clientFd);
            } else {
                closeFd(clientFd);
            }
        }
    }

private:
    bool createListenSocket() {
        if (backendType_ == EpollBackendType::kFStack) {
#if defined(FSTACK_ENABLE_BACKEND)
            listenFd_ = ff_socket(AF_INET, SOCK_STREAM, 0);
#else
            errno = ENOSYS;
            listenFd_ = -1;
#endif
        } else {
            listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        }
        return listenFd_ >= 0;
    }

    void setReuseAddr() {
        int on = 1;
        if (backendType_ == EpollBackendType::kFStack) {
#if defined(FSTACK_ENABLE_BACKEND)
            (void)ff_setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
        } else {
            (void)::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        }
    }

    bool bindAddress(const std::string& ip, uint16_t port) {
        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
            errno = EINVAL;
            return false;
        }

        if (backendType_ == EpollBackendType::kFStack) {
#if defined(FSTACK_ENABLE_BACKEND)
            return ff_bind(listenFd_, reinterpret_cast<linux_sockaddr*>(&addr), sizeof(addr)) == 0;
#else
            errno = ENOSYS;
            return false;
#endif
        }

        return ::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    bool listenInternal(int backlog) {
        if (backendType_ == EpollBackendType::kFStack) {
#if defined(FSTACK_ENABLE_BACKEND)
            return ff_listen(listenFd_, backlog) == 0;
#else
            errno = ENOSYS;
            return false;
#endif
        }

        return ::listen(listenFd_, backlog) == 0;
    }

    int acceptOne() {
        if (backendType_ == EpollBackendType::kFStack) {
#if defined(FSTACK_ENABLE_BACKEND)
            return ff_accept(listenFd_, nullptr, nullptr);
#else
            errno = ENOSYS;
            return -1;
#endif
        }

        return ::accept(listenFd_, nullptr, nullptr);
    }

    bool setNonBlocking(int fd) {
        if (backendType_ == EpollBackendType::kFStack) {
#if defined(FSTACK_ENABLE_BACKEND)
            int on = 1;
            return ff_ioctl(fd, FIONBIO, &on) == 0;
#else
            (void)fd;
            errno = ENOSYS;
            return false;
#endif
        }

        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0) {
            return false;
        }
        return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    void closeFd(int fd) {
        if (backendType_ == EpollBackendType::kFStack) {
#if defined(FSTACK_ENABLE_BACKEND)
            (void)ff_close(fd);
#else
            (void)::close(fd);
#endif
        } else {
            (void)::close(fd);
        }
    }

private:
    EpollBackendType backendType_;
    int listenFd_;
    NewConnectionCallback callback_;
};