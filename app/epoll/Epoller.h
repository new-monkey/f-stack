#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sys/epoll.h>
#include <unistd.h>
#include <vector>

#if defined(FSTACK_ENABLE_BACKEND)
#include "../../lib/ff_epoll.h"
#endif

enum class EpollBackendType {
	kKernel,
	kFStack,
};

class IEpollBackend {
public:
	virtual ~IEpollBackend() = default;

	virtual int create(int sizeHint) = 0;
	virtual int ctl(int epfd, int op, int fd, struct epoll_event* event) = 0;
	virtual int wait(int epfd, struct epoll_event* events, int maxEvents, int timeoutMs) = 0;
	virtual int closeFd(int fd) = 0;
};

class KernelEpollBackend final : public IEpollBackend {
public:
	int create(int sizeHint) override {
		(void)sizeHint;
#ifdef EPOLL_CLOEXEC
		return ::epoll_create1(EPOLL_CLOEXEC);
#else
		if (sizeHint <= 0) {
		    sizeHint = 1;
		}
		return ::epoll_create(sizeHint);
#endif
	}

	int ctl(int epfd, int op, int fd, struct epoll_event* event) override {
		return ::epoll_ctl(epfd, op, fd, event);
	}

	int wait(int epfd, struct epoll_event* events, int maxEvents, int timeoutMs) override {
		return ::epoll_wait(epfd, events, maxEvents, timeoutMs);
	}

	int closeFd(int fd) override { return ::close(fd); }
};

class FStackEpollBackend final : public IEpollBackend {
public:
	int create(int sizeHint) override {
#if defined(FSTACK_ENABLE_BACKEND)
		return ff_epoll_create(sizeHint);
#else
		(void)sizeHint;
		errno = ENOSYS;
		return -1;
#endif
	}

	int ctl(int epfd, int op, int fd, struct epoll_event* event) override {
#if defined(FSTACK_ENABLE_BACKEND)
		return ff_epoll_ctl(epfd, op, fd, event);
#else
		(void)epfd;
		(void)op;
		(void)fd;
		(void)event;
		errno = ENOSYS;
		return -1;
#endif
	}

	int wait(int epfd, struct epoll_event* events, int maxEvents, int timeoutMs) override {
#if defined(FSTACK_ENABLE_BACKEND)
		return ff_epoll_wait(epfd, events, maxEvents, timeoutMs);
#else
		(void)epfd;
		(void)events;
		(void)maxEvents;
		(void)timeoutMs;
		errno = ENOSYS;
		return -1;
#endif
	}

	int closeFd(int fd) override { return ::close(fd); }
};

class Epoller {
public:
	explicit Epoller(EpollBackendType backendType, std::size_t maxEvents = 1024)
		: backend_(createBackend(backendType)),
		  maxEvents_(maxEvents == 0 ? 1 : maxEvents),
		  events_(maxEvents_),
		  epfd_(-1) {}

	~Epoller() {
		if (epfd_ >= 0) {
			(void)backend_->closeFd(epfd_);
		}
	}

	Epoller(const Epoller&) = delete;
	Epoller& operator=(const Epoller&) = delete;

	bool open() {
		if (epfd_ >= 0) {
			return true;
		}
		epfd_ = backend_->create(static_cast<int>(maxEvents_));
		return epfd_ >= 0;
	}

	int fd() const { return epfd_; }

	bool add(int fd, uint32_t events, void* userData = nullptr) {
		struct epoll_event ev;
		ev.events = events;
		if (userData != nullptr) {
			ev.data.ptr = userData;
		} else {
			ev.data.fd = fd;
		}
		return backend_->ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == 0;
	}

	bool mod(int fd, uint32_t events, void* userData = nullptr) {
		struct epoll_event ev;
		ev.events = events;
		if (userData != nullptr) {
			ev.data.ptr = userData;
		} else {
			ev.data.fd = fd;
		}
		return backend_->ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0;
	}

	bool del(int fd) {
		struct epoll_event ev;
		ev.events = 0;
		ev.data.fd = fd;
		return backend_->ctl(epfd_, EPOLL_CTL_DEL, fd, &ev) == 0;
	}

	int wait(int timeoutMs) {
		return backend_->wait(epfd_, events_.data(), static_cast<int>(events_.size()), timeoutMs);
	}

	const struct epoll_event& eventAt(std::size_t index) const { return events_[index]; }

private:
	static std::unique_ptr<IEpollBackend> createBackend(EpollBackendType type) {
		if (type == EpollBackendType::kFStack) {
			return std::unique_ptr<IEpollBackend>(new FStackEpollBackend());
		}
		return std::unique_ptr<IEpollBackend>(new KernelEpollBackend());
	}

private:
	std::unique_ptr<IEpollBackend> backend_;
	std::size_t maxEvents_;
	std::vector<struct epoll_event> events_;
	int epfd_;
};
