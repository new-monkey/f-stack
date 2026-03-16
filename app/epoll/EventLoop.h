#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <sys/epoll.h>
#include <unistd.h>
#include <unordered_map>

#include "Epoller.h"
#include "MessageDispatcher.h"
#include "TcpConnection.h"

#if defined(FSTACK_ENABLE_BACKEND)
#include "../../lib/ff_api.h"
#endif

class EventLoop {
public:
	struct Options {
		std::size_t maxEvents = 1024;
		std::size_t maxPayloadBytes = 40960;
		std::size_t maxOutputBufferBytes = 1U << 20;
		std::size_t maxReadItersPerEvent = 64;
		std::size_t maxWriteItersPerEvent = 64;
		TcpConnection::OverflowPolicy overflowPolicy = TcpConnection::OverflowPolicy::kDropNewData;
	};

	explicit EventLoop(EpollBackendType backendType)
		: EventLoop(backendType, Options()) {}

	EventLoop(EpollBackendType backendType, const Options& options)
		: epoller_(backendType, options.maxEvents),
		  backendType_(backendType),
		  options_(options),
		  running_(false) {}

	bool init() { return epoller_.open(); }

	MessageDispatcher& dispatcher() { return dispatcher_; }

	void queueInLoop(std::function<void()> fn) {
		if (fn) {
			pendingFunctors_.push_back(std::move(fn));
		}
	}

	void runInLoop(std::function<void()> fn) { queueInLoop(std::move(fn)); }

	bool addConnection(int fd) {
		auto conn = std::make_shared<TcpConnection>(fd, buildIoOps(), &dispatcher_, buildConnOptions());
		conn->setCloseCallback([this](int closedFd) { connections_.erase(closedFd); });
		conn->markConnected();

		if (!epoller_.add(fd, conn->interestedEvents(), conn.get())) {
			return false;
		}

		connections_[fd] = std::move(conn);
		return true;
	}

	void removeConnection(int fd) {
		(void)epoller_.del(fd);
		const auto it = connections_.find(fd);
		if (it == connections_.end()) {
			return;
		}
		it->second->shutdown();
		connections_.erase(it);
	}

	int loopOnce(int timeoutMs) {
		const int n = epoller_.wait(timeoutMs);
		if (n <= 0) {
			return n;
		}

		for (int i = 0; i < n; ++i) {
			const struct epoll_event& ev = epoller_.eventAt(static_cast<std::size_t>(i));
			auto* connPtr = static_cast<TcpConnection*>(ev.data.ptr);
			if (connPtr == nullptr) {
				continue;
			}

			const int fd = connPtr->fd();
			const auto mapIt = connections_.find(fd);
			if (mapIt == connections_.end()) {
				continue;
			}

			const auto conn = mapIt->second;
			if (ev.events & static_cast<uint32_t>(EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
				conn->shutdown();
				(void)epoller_.del(fd);
				continue;
			}

			if (ev.events & static_cast<uint32_t>(EPOLLIN)) {
				conn->handleReadEvent();
			}

			if (conn->state() == TcpConnection::State::kDisconnected) {
				(void)epoller_.del(fd);
				continue;
			}

			if (ev.events & static_cast<uint32_t>(EPOLLOUT)) {
				conn->handleWriteEvent();
			}

			if (conn->state() == TcpConnection::State::kDisconnected) {
				(void)epoller_.del(fd);
				continue;
			}

			(void)epoller_.mod(fd, conn->interestedEvents(), conn.get());
		}

		return n;
	}

	void run(int timeoutMs = 1000) {
		running_ = true;
		while (running_) {
			doPendingFunctors();
			const int n = loopOnce(timeoutMs);
			doPendingFunctors();
			if (n < 0 && errno != EINTR) {
				break;
			}
		}
	}

	void stop() { running_ = false; }

private:
	TcpConnection::IoOps buildIoOps() const {
		TcpConnection::IoOps ops;
		if (backendType_ == EpollBackendType::kFStack) {
#if defined(FSTACK_ENABLE_BACKEND)
			ops.readFn = [](int fd, void* buf, size_t len) { return ff_read(fd, buf, len); };
			ops.writeFn = [](int fd, const void* buf, size_t len) { return ff_write(fd, buf, len); };
			ops.closeFn = [](int fd) { return ff_close(fd); };
#else
			ops.readFn = [](int fd, void* buf, size_t len) {
				(void)fd;
				(void)buf;
				(void)len;
				errno = ENOSYS;
				return static_cast<ssize_t>(-1);
			};
			ops.writeFn = [](int fd, const void* buf, size_t len) {
				(void)fd;
				(void)buf;
				(void)len;
				errno = ENOSYS;
				return static_cast<ssize_t>(-1);
			};
			ops.closeFn = [](int fd) {
				(void)fd;
				errno = ENOSYS;
				return -1;
			};
#endif
		} else {
			ops.readFn = [](int fd, void* buf, size_t len) { return ::read(fd, buf, len); };
			ops.writeFn = [](int fd, const void* buf, size_t len) { return ::write(fd, buf, len); };
			ops.closeFn = [](int fd) { return ::close(fd); };
		}
		return ops;
	}

	TcpConnection::Options buildConnOptions() const {
		TcpConnection::Options connOptions;
		connOptions.maxPayloadBytes = options_.maxPayloadBytes;
		connOptions.maxOutputBufferBytes = options_.maxOutputBufferBytes;
		connOptions.maxReadItersPerEvent = options_.maxReadItersPerEvent;
		connOptions.maxWriteItersPerEvent = options_.maxWriteItersPerEvent;
		connOptions.overflowPolicy = options_.overflowPolicy;
		return connOptions;
	}

	void doPendingFunctors() {
		if (pendingFunctors_.empty()) {
			return;
		}

		auto pending = std::move(pendingFunctors_);
		pendingFunctors_.clear();
		for (auto& fn : pending) {
			if (fn) {
				fn();
			}
		}
	}

private:
	Epoller epoller_;
	EpollBackendType backendType_;
	Options options_;
	MessageDispatcher dispatcher_;
	std::unordered_map<int, std::shared_ptr<TcpConnection>> connections_;
	std::deque<std::function<void()>> pendingFunctors_;
	bool running_;
};
