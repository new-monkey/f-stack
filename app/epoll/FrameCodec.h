#pragma once

#include <arpa/inet.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

class FrameCodec {
public:
	struct Header {
		uint32_t framePayloadLen = 0;  // FramePayload bytes, includes MsgCode
		uint32_t msgCode = 0;
	};

	static constexpr std::size_t kFrameLenFieldSize = sizeof(uint32_t);
	static constexpr std::size_t kMsgCodeSize = sizeof(uint32_t);
	static constexpr std::size_t kHeaderSize = kFrameLenFieldSize + kMsgCodeSize;
	static constexpr uint32_t kMinFramePayloadLen = static_cast<uint32_t>(kMsgCodeSize);
	static constexpr uint32_t kMinPayloadLen = kMinFramePayloadLen;  // backward-compatible alias

	static bool decodeHeader(std::string_view headerBytes, Header& out) {
		if (headerBytes.size() < kHeaderSize) {
			return false;
		}

		uint32_t netLen = 0;
		uint32_t netCode = 0;
		std::memcpy(&netLen, headerBytes.data(), sizeof(netLen));
		std::memcpy(&netCode, headerBytes.data() + sizeof(netLen), sizeof(netCode));
		out.framePayloadLen = ntohl(netLen);
		out.msgCode = ntohl(netCode);
		return true;
	}

	static bool isFramePayloadLenValid(uint32_t framePayloadLen, std::size_t maxPayloadBytes) {
		return framePayloadLen >= kMinFramePayloadLen && framePayloadLen <= maxPayloadBytes;
	}

	static bool isPayloadLenValid(uint32_t payloadLen, std::size_t maxPayloadBytes) {
		return isFramePayloadLenValid(payloadLen, maxPayloadBytes);
	}

	static std::size_t totalFrameBytes(uint32_t framePayloadLen) {
		return kFrameLenFieldSize + static_cast<std::size_t>(framePayloadLen);
	}

	static std::size_t bodyBytesFromFramePayloadLen(uint32_t framePayloadLen) {
		return static_cast<std::size_t>(framePayloadLen - kMsgCodeSize);
	}

	static std::size_t bodyBytesFromPayloadLen(uint32_t payloadLen) {
		return bodyBytesFromFramePayloadLen(payloadLen);
	}

	static std::string_view payloadFromFrame(std::string_view frameBytes, uint32_t framePayloadLen) {
		return std::string_view(frameBytes.data() + kFrameLenFieldSize, framePayloadLen);
	}

	static bool extractMsgCode(std::string_view payload, uint32_t& outMsgCode) {
		if (payload.size() < kMsgCodeSize) {
			return false;
		}
		uint32_t netCode = 0;
		std::memcpy(&netCode, payload.data(), sizeof(netCode));
		outMsgCode = ntohl(netCode);
		return true;
	}

	static std::string_view bodyFromPayload(std::string_view payload) {
		if (payload.size() < kMsgCodeSize) {
			return std::string_view();
		}
		return payload.substr(kMsgCodeSize);
	}

	static std::string encode(uint32_t msgCode, std::string_view body) {
		const uint32_t payloadLenHost = static_cast<uint32_t>(kMsgCodeSize + body.size());
		const uint32_t netLen = htonl(payloadLenHost);
		const uint32_t netCode = htonl(msgCode);

		std::string frame;
		frame.resize(kHeaderSize + body.size());
		std::memcpy(frame.data(), &netLen, sizeof(netLen));
		std::memcpy(frame.data() + sizeof(netLen), &netCode, sizeof(netCode));
		if (!body.empty()) {
			std::memcpy(frame.data() + kHeaderSize, body.data(), body.size());
		}
		return frame;
	}

	// Encode when payload is already built as [MsgCode|BusinessHeader|Body].
	static std::string encode(std::string_view payload) {
		const uint32_t payloadLenHost = static_cast<uint32_t>(payload.size());
		const uint32_t netLen = htonl(payloadLenHost);

		std::string frame;
		frame.resize(kFrameLenFieldSize + payload.size());
		std::memcpy(frame.data(), &netLen, sizeof(netLen));
		if (!payload.empty()) {
			std::memcpy(frame.data() + kFrameLenFieldSize, payload.data(), payload.size());
		}
		return frame;
	}

};