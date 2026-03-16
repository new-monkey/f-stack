#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "FrameCodec.h"

namespace {

bool sendAll(int fd, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool recvAll(int fd, char* data, std::size_t len) {
    std::size_t recvd = 0;
    while (recvd < len) {
        const ssize_t n = ::recv(fd, data + recvd, len - recvd, 0);
        if (n > 0) {
            recvd += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool decodeResponse(int fd, uint32_t& outHeaderCode, uint32_t& outPayloadCode, std::string& outBody) {
    char header[8];
    if (!recvAll(fd, header, sizeof(header))) {
        return false;
    }

    FrameCodec::Header decodedHeader;
    if (!FrameCodec::decodeHeader(std::string_view(header, sizeof(header)), decodedHeader)) {
        return false;
    }

    outHeaderCode = decodedHeader.msgCode;
    if (decodedHeader.framePayloadLen < FrameCodec::kMinFramePayloadLen) {
        return false;
    }

    // payloadLen includes MsgCode(4B), but MsgCode has already been read in the 8B header.
    const std::size_t bodyLen = FrameCodec::bodyBytesFromFramePayloadLen(decodedHeader.framePayloadLen);
    std::string body(bodyLen, '\0');
    if (bodyLen > 0 && !recvAll(fd, body.data(), body.size())) {
        return false;
    }

    outPayloadCode = outHeaderCode;
    outBody = std::move(body);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    uint16_t port = 19091;
    uint32_t msgCode = 1;
    std::string requestBody = "GET /index.html";

    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = static_cast<uint16_t>(std::stoi(argv[2]));
    }
    if (argc >= 4) {
        msgCode = static_cast<uint32_t>(std::stoul(argv[3]));
    }
    if (argc >= 5) {
        requestBody = argv[4];
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return 1;
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "invalid host: " << host << std::endl;
        ::close(fd);
        return 1;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::perror("connect");
        ::close(fd);
        return 1;
    }

    const std::string frame = FrameCodec::encode(msgCode, requestBody);
    if (!sendAll(fd, frame.data(), frame.size())) {
        std::perror("sendAll");
        ::close(fd);
        return 1;
    }

    uint32_t headerCode = 0;
    uint32_t payloadCode = 0;
    std::string html;
    if (!decodeResponse(fd, headerCode, payloadCode, html)) {
        std::cerr << "decode response failed" << std::endl;
        ::close(fd);
        return 1;
    }

    std::cout << "response header code: " << headerCode << std::endl;
    std::cout << "response payload code: " << payloadCode << std::endl;
    std::cout << "response body:" << std::endl;
    std::cout << html << std::endl;

    if (headerCode != payloadCode) {
        std::cerr << "protocol mismatch: header code != payload code" << std::endl;
        ::close(fd);
        return 2;
    }

    ::close(fd);
    return 0;
}
