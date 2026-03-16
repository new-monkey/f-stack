#pragma once

#include <cstdint>
#include <string>
#include <unistd.h>

#include "Acceptor.h"
#include "EventLoop.h"

class ReactorServer {
public:
	ReactorServer(EpollBackendType backendType, EventLoop::Options options = EventLoop::Options())
		: loop_(backendType, options), acceptor_(backendType) {}

	bool init(const std::string& ip, uint16_t port, int backlog = 1024) {
		if (!loop_.init()) {
			return false;
		}

		if (!acceptor_.open(ip, port, backlog)) {
			return false;
		}

		acceptor_.setNewConnectionCallback([this](int clientFd) {
			if (!loop_.addConnection(clientFd)) {
				(void)::close(clientFd);
			}
		});

		return true;
	}

	MessageDispatcher& dispatcher() { return loop_.dispatcher(); }

	EventLoop& loop() { return loop_; }

	const EventLoop::Stats& stats() const { return loop_.stats(); }

	void serveOnce(int timeoutMs) {
		acceptor_.acceptReady();
		(void)loop_.loopOnce(timeoutMs);
	}

	void run(int timeoutMs = 1000) {
		for (;;) {
			serveOnce(timeoutMs);
		}
	}

private:
	EventLoop loop_;
	Acceptor acceptor_;
};