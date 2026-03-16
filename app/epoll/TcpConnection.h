#pragma once

#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <cstring>
#include <sys/epoll.h>

#include "Buffer.h"
#include "MessageDispatcher.h"

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
	struct Stats {
		std::size_t bytesRead = 0;
		std::size_t bytesWritten = 0;
		std::size_t framesDispatched = 0;
		std::size_t droppedBytes = 0;
		std::size_t protocolErrors = 0;
	};

	enum class State {
		kConnecting,
		kConnected,
		kDisconnecting,
		kDisconnected,
	};

	enum class OverflowPolicy {
		kDropNewData,
		kDropOldData,
	};

	struct IoOps {
		std::function<ssize_t(int, void*, size_t)> readFn;
		std::function<ssize_t(int, const void*, size_t)> writeFn;
		std::function<int(int)> closeFn;
	};

	struct Options {
		std::size_t maxPayloadBytes = 40960;
		std::size_t maxOutputBufferBytes = 1U << 20;
		std::size_t maxReadItersPerEvent = 64;
		std::size_t maxWriteItersPerEvent = 64;
		OverflowPolicy overflowPolicy = OverflowPolicy::kDropNewData;
	};

	using CloseCallback = std::function<void(const TcpConnection&)>;

	TcpConnection(int fd, IoOps ioOps, MessageDispatcher* dispatcher)
		: TcpConnection(fd, std::move(ioOps), dispatcher, Options()) {}

	TcpConnection(int fd, IoOps ioOps, MessageDispatcher* dispatcher, const Options& options)
		: fd_(fd),
		  ioOps_(std::move(ioOps)),
		  dispatcher_(dispatcher),
		  options_(options),
		  state_(State::kConnecting),
		  interestedEvents_(0),
		  closeNotified_(false) {}

	~TcpConnection() {
		if (fd_ >= 0) {
			(void)ioOps_.closeFn(fd_);
			fd_ = -1;
		}
	}

	int fd() const { return fd_; }

	State state() const { return state_; }

	uint32_t interestedEvents() const { return interestedEvents_; }

	std::size_t droppedBytes() const { return stats_.droppedBytes; }

	std::size_t protocolErrorCount() const { return stats_.protocolErrors; }

	std::size_t bytesRead() const { return stats_.bytesRead; }

	std::size_t bytesWritten() const { return stats_.bytesWritten; }

	std::size_t framesDispatched() const { return stats_.framesDispatched; }

	const Stats& stats() const { return stats_; }

	void setCloseCallback(CloseCallback cb) { closeCallback_ = std::move(cb); }

	void markConnected() {
		state_ = State::kConnected;
		interestedEvents_ = static_cast<uint32_t>(EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP);
	}

	bool send(std::string_view data) {
		if (state_ != State::kConnected && state_ != State::kDisconnecting) {
			return false;
		}

		if (data.empty()) {
			return true;
		}

		if (outputBuffer_.readableBytes() == 0) {
			const ssize_t n = ioOps_.writeFn(fd_, data.data(), data.size());
			if (n >= 0) {
				stats_.bytesWritten += static_cast<std::size_t>(n);
				if (static_cast<std::size_t>(n) == data.size()) {
					disableWriteEvent();
					return true;
				}

				const std::string_view remain = data.substr(static_cast<std::size_t>(n));
				return appendToOutputBuffer(remain);
			}

			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				shutdown();
				return false;
			}
		}

		return appendToOutputBuffer(data);
	}

	void handleReadEvent() {
		if (state_ != State::kConnected && state_ != State::kDisconnecting) {
			return;
		}

		for (std::size_t i = 0; i < options_.maxReadItersPerEvent; ++i) {
			char tmp[8192];
			const ssize_t n = ioOps_.readFn(fd_, tmp, sizeof(tmp));
			if (n > 0) {
				stats_.bytesRead += static_cast<std::size_t>(n);
				inputBuffer_.append(tmp, static_cast<std::size_t>(n));
				continue;
			}

			if (n == 0) {
				shutdown();
				break;
			}

			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				break;
			}

			shutdown();
			break;
		}

		parseFrames();
	}

	void handleWriteEvent() {
		if (state_ != State::kConnected && state_ != State::kDisconnecting) {
			return;
		}

		for (std::size_t i = 0; i < options_.maxWriteItersPerEvent; ++i) {
			if (outputBuffer_.readableBytes() == 0) {
				disableWriteEvent();
				break;
			}

			const std::string_view chunk = outputBuffer_.peekAsView(outputBuffer_.readableBytes());
			const ssize_t n = ioOps_.writeFn(fd_, chunk.data(), chunk.size());
			if (n > 0) {
				stats_.bytesWritten += static_cast<std::size_t>(n);
				outputBuffer_.retrieve(static_cast<std::size_t>(n));
				continue;
			}

			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				enableWriteEvent();
				break;
			}

			shutdown();
			break;
		}
	}

	void shutdown() {
		if (state_ == State::kDisconnected) {
			return;
		}

		state_ = State::kDisconnected;
		interestedEvents_ = 0;

		if (!closeNotified_ && closeCallback_) {
			closeNotified_ = true;
			closeCallback_(*this);
		}
	}

private:
	static constexpr std::size_t kHeaderLen = 8;
	static constexpr std::size_t kFrameHeaderLen = 4;

	bool appendToOutputBuffer(std::string_view data) {
		if (data.empty()) {
			return true;
		}

		if (options_.maxOutputBufferBytes == 0) {
			stats_.droppedBytes += data.size();
			return false;
		}

		if (outputBuffer_.readableBytes() + data.size() <= options_.maxOutputBufferBytes) {
			outputBuffer_.append(data);
			enableWriteEvent();
			return true;
		}

		const std::size_t overflow = outputBuffer_.readableBytes() + data.size() - options_.maxOutputBufferBytes;
		stats_.droppedBytes += overflow;

		if (options_.overflowPolicy == OverflowPolicy::kDropNewData) {
			const std::size_t canKeep = options_.maxOutputBufferBytes > outputBuffer_.readableBytes()
											? options_.maxOutputBufferBytes - outputBuffer_.readableBytes()
											: 0;
			if (canKeep > 0) {
				outputBuffer_.append(data.data(), canKeep);
				enableWriteEvent();
			}
			return false;
		}

		if (overflow >= outputBuffer_.readableBytes()) {
			outputBuffer_.retrieveAll();
		} else {
			outputBuffer_.retrieve(overflow);
		}

		if (data.size() > options_.maxOutputBufferBytes) {
			const std::size_t start = data.size() - options_.maxOutputBufferBytes;
			outputBuffer_.append(data.substr(start));
			enableWriteEvent();
			return false;
		}

		outputBuffer_.append(data);
		enableWriteEvent();
		return false;
	}

	// [PayloadLen:4B][MsgCode:4B][PayloadBody:payloadLenB-4B]
	// PayloadLen = sizeof(MsgCode) + sizeof(PayloadBody)
	// Dispatcher payload view = [MsgCode|PayloadBody] (length=PayloadLen)
	void parseFrames() {
		while (inputBuffer_.readableBytes() >= kHeaderLen) {
			const std::string_view header = inputBuffer_.peekAsView(kHeaderLen);

			uint32_t netLen = 0;
			uint32_t netCode = 0;
			std::memcpy(&netLen, header.data(), sizeof(netLen));
			std::memcpy(&netCode, header.data() + sizeof(netLen), sizeof(netCode));

			const uint32_t payloadLen = ntohl(netLen);
			const uint32_t msgCode = ntohl(netCode);

			if (payloadLen < sizeof(uint32_t) || payloadLen > options_.maxPayloadBytes) {
				++stats_.protocolErrors;
				inputBuffer_.retrieve(kFrameHeaderLen);
				continue;
			}

			const std::size_t frameLen = kFrameHeaderLen + static_cast<std::size_t>(payloadLen);
			if (inputBuffer_.readableBytes() < frameLen) {
				return;
			}

			const std::string_view frame = inputBuffer_.peekAsView(frameLen);
			const std::string_view payload(frame.data() + kFrameHeaderLen, payloadLen);
			if (dispatcher_ != nullptr) {
				dispatcher_->dispatch(msgCode, payload, shared_from_this());
			}
			++stats_.framesDispatched;
			inputBuffer_.retrieve(frameLen);
		}
	}

	void enableWriteEvent() {
		interestedEvents_ |= static_cast<uint32_t>(EPOLLOUT);
		interestedEvents_ |= static_cast<uint32_t>(EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP);
	}

	void disableWriteEvent() {
		interestedEvents_ &= ~static_cast<uint32_t>(EPOLLOUT);
		interestedEvents_ |= static_cast<uint32_t>(EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP);
	}

private:
	int fd_;
	IoOps ioOps_;
	MessageDispatcher* dispatcher_;
	Options options_;
	State state_;
	uint32_t interestedEvents_;
	Buffer inputBuffer_;
	Buffer outputBuffer_;
	Stats stats_;
	bool closeNotified_;
	CloseCallback closeCallback_;
};
