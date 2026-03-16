#!/usr/bin/env python3

import argparse
import socket
import struct
import sys


def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("connection closed by peer")
        data.extend(chunk)
    return bytes(data)


def encode_frame(msg_code: int, body: bytes) -> bytes:
    frame_payload_len = 4 + len(body)
    return struct.pack("!II", frame_payload_len, msg_code) + body


def decode_frame(sock: socket.socket) -> tuple[int, bytes]:
    header = recv_exact(sock, 8)
    frame_payload_len, msg_code = struct.unpack("!II", header)
    if frame_payload_len < 4:
        raise ValueError(f"invalid frame payload len: {frame_payload_len}")
    body_len = frame_payload_len - 4
    body = recv_exact(sock, body_len) if body_len > 0 else b""
    return msg_code, body


def format_body(body: bytes) -> str:
    try:
        return body.decode("utf-8")
    except UnicodeDecodeError:
        return body.hex()


def print_help() -> None:
    print("commands:")
    print("  /help           show this help")
    print("  /code <num>     change current msgCode")
    print("  /hex <hexstr>   send raw hex body bytes using current msgCode")
    print("  /quit           exit client")
    print("  any other line  send as UTF-8 body using current msgCode")


def interactive_loop(sock: socket.socket, msg_code: int) -> int:
    current_code = msg_code
    print(f"connected, current msgCode={current_code}")
    print_help()

    while True:
        try:
            line = input("echo> ").strip()
        except EOFError:
            print()
            return 0
        except KeyboardInterrupt:
            print()
            return 0

        if not line:
            continue
        if line in {"/quit", "/exit"}:
            return 0
        if line == "/help":
            print_help()
            continue
        if line.startswith("/code "):
            value = line.split(maxsplit=1)[1]
            try:
                current_code = int(value, 10)
            except ValueError:
                print(f"invalid msgCode: {value}")
                continue
            print(f"current msgCode={current_code}")
            continue

        if line.startswith("/hex "):
            value = line.split(maxsplit=1)[1].replace(" ", "")
            try:
                body = bytes.fromhex(value)
            except ValueError as exc:
                print(f"invalid hex: {exc}")
                continue
        else:
            body = line.encode("utf-8")

        frame = encode_frame(current_code, body)
        try:
            sock.sendall(frame)
            response_code, response_body = decode_frame(sock)
        except (ConnectionError, OSError, ValueError) as exc:
            print(f"request failed: {exc}")
            return 1

        print(f"response msgCode={response_code}")
        print(f"response body={format_body(response_body)}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Interactive client for dev_kernel_echo_server")
    parser.add_argument("host", nargs="?", default="127.0.0.1")
    parser.add_argument("port", nargs="?", type=int, default=19090)
    parser.add_argument("--code", type=int, default=1, help="initial msgCode")
    parser.add_argument("--timeout", type=float, default=5.0, help="socket timeout in seconds")
    args = parser.parse_args()

    try:
        with socket.create_connection((args.host, args.port), timeout=args.timeout) as sock:
            sock.settimeout(args.timeout)
            return interactive_loop(sock, args.code)
    except OSError as exc:
        print(f"connect failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())