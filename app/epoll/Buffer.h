#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

class Buffer {
public:
	static constexpr std::size_t kDefaultInitialSize = 4096;

	explicit Buffer(std::size_t initialSize = kDefaultInitialSize)
		: buffer_(initialSize), readIndex_(0), writeIndex_(0) {}

	std::size_t readableBytes() const { return writeIndex_ - readIndex_; }

	std::size_t writableBytes() const { return buffer_.size() - writeIndex_; }

	std::size_t prependableBytes() const { return readIndex_; }

	const char* peek() const { return begin() + readIndex_; }

	std::string_view peekAsView(std::size_t len) const {
		if (len > readableBytes()) {
			len = readableBytes();
		}
		return std::string_view(peek(), len);
	}

	void retrieve(std::size_t len) {
		if (len >= readableBytes()) {
			retrieveAll();
			return;
		}
		readIndex_ += len;
	}

	void retrieveAll() {
		readIndex_ = 0;
		writeIndex_ = 0;
	}

	std::string retrieveAsString(std::size_t len) {
		len = std::min(len, readableBytes());
		std::string result(peek(), len);
		retrieve(len);
		return result;
	}

	std::string retrieveAllAsString() { return retrieveAsString(readableBytes()); }

	char* beginWrite() { return begin() + writeIndex_; }

	const char* beginWrite() const { return begin() + writeIndex_; }

	void hasWritten(std::size_t len) {
		assert(len <= writableBytes());
		writeIndex_ += len;
	}

	void unwrite(std::size_t len) {
		assert(len <= readableBytes());
		writeIndex_ -= len;
	}

	void ensureWritableBytes(std::size_t len) {
		if (writableBytes() >= len) {
			return;
		}
		makeSpace(len);
	}

	void append(const char* data, std::size_t len) {
		if (len == 0) {
			return;
		}
		ensureWritableBytes(len);
		std::memcpy(beginWrite(), data, len);
		hasWritten(len);
	}

	void append(std::string_view data) { append(data.data(), data.size()); }

private:
	char* begin() { return buffer_.data(); }

	const char* begin() const { return buffer_.data(); }

	void makeSpace(std::size_t len) {
		if (writableBytes() + prependableBytes() < len) {
			buffer_.resize(writeIndex_ + len);
			return;
		}

		const std::size_t readable = readableBytes();
		std::memmove(begin(), peek(), readable);
		readIndex_ = 0;
		writeIndex_ = readable;
	}

private:
	std::vector<char> buffer_;
	std::size_t readIndex_;
	std::size_t writeIndex_;
};
