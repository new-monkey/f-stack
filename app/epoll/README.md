# app/epoll 开发说明

术语规范入口：`TERMINOLOGY.md`

本目录提供 Reactor 组件的基础实现，支持两种后端：

- Kernel 后端：使用 `epoll_*` + `read/write`，用于开发阶段替代测试。
- F-Stack 后端：使用 `ff_epoll_*` + `ff_read/ff_write`，用于最终集成。

核心组合关系：

- `ReactorServer` 组合 `Acceptor + EventLoop`
- `EventLoop` 管理 `TcpConnection + MessageDispatcher`
- `TcpConnection` 持有输入输出 `Buffer`
- `FrameCodec` 统一处理传输层帧编码/解码

## 后端切换

通过 `EpollBackendType` 在运行期选择后端：

- `EpollBackendType::kKernel`
- `EpollBackendType::kFStack`

同时，F-Stack 代码路径默认是编译期可选的：

- 若定义 `FSTACK_ENABLE_BACKEND`，会启用真实 `ff_*` 调用。
- 若不定义该宏，`kFStack` 分支返回 `ENOSYS`，可保证 kernel-only 链接通过。

## 开发期最小可运行示例

示例文件：`dev_kernel_echo_server.cpp`

交互式测试客户端：`dev_echo_client.py`

- 使用 `Acceptor` 组件负责 listen/accept 与非阻塞设置。
- 使用 `EventLoop` + `TcpConnection` 处理读写与消息分发。
- 新版本可直接通过 `ReactorServer` 启动，业务层只需要注册 dispatcher handler。

编译：

```bash
g++ -std=c++17 -I/root/f-stack -I/root/f-stack/lib app/epoll/dev_kernel_echo_server.cpp -o /tmp/dev_kernel_echo_server
```

运行：

```bash
/tmp/dev_kernel_echo_server 19090
python3 app/epoll/dev_echo_client.py 127.0.0.1 19090
```

协议格式：

- Frame: `FrameLen(4 bytes, network order) + FramePayload`
- FramePayload: `MsgCode(4 bytes, network order) + BusinessHeader + MsgBody`
- `FrameLen = sizeof(FramePayload)`，不包含前 4 字节长度头
- `sizeof(FramePayload) <= 40960`

`TcpConnection` 在分发时的 `framePayload` 视图定义：

- `framePayload` 从 `MsgCode` 开始，长度为 `FrameLen`
- 业务层可通过 `FrameCodec::extractMsgCode` 和 `FrameCodec::bodyFromPayload` 继续拆分

统一编解码入口（`FrameCodec.h`）：

- `FrameCodec::encode(msgCode, body)`：将业务 body 编码为 `FrameLen + MsgCode + Body`
- `FrameCodec::encode(payload)`：当业务层已编码好 `FramePayload`（已包含 `MsgCode`）时直接封帧
- `FrameCodec::decodeHeader(...)`：解码固定 8 字节头
- `FrameCodec::isPayloadLenValid(...)`：统一长度合法性校验
- `FrameCodec::totalFrameBytes(...)`：整帧字节数计算

若业务不是帧协议，而是原始字节流（例如简单 HTTP 示例），可将 `TcpConnection::Options::enableFrameCodec` 设为 `false`，并在连接建立后调用 `setRawMessageCallback(...)` 处理读到的原始数据。

默认注册：

- `MsgCode == 1`：回显 payload。

交互式客户端命令：

- `/code <num>`：切换当前发送的 `MsgCode`
- `/hex <hexstr>`：按十六进制发送 body
- `/quit`：退出
- 其他输入：按 UTF-8 文本作为 body 发送

## ReactorServer HTML 变体与专用客户端

示例文件：

- 服务端：`dev_reactor_html_server.cpp`
- 客户端：`dev_reactor_html_client.cpp`

说明：

- 服务端基于 `ReactorServer`，通过回调注入分发处理逻辑，返回 HTML 字符串。
- 客户端不是通用 HTTP 客户端，而是协议对齐的帧客户端，用于发送/接收 `FrameLen + FramePayload`。

编译：

```bash
g++ -std=c++17 -I/root/f-stack -I/root/f-stack/lib app/epoll/dev_reactor_html_server.cpp -o /tmp/dev_reactor_html_server
g++ -std=c++17 -I/root/f-stack -I/root/f-stack/lib app/epoll/dev_reactor_html_client.cpp -o /tmp/dev_reactor_html_client
```

运行：

```bash
/tmp/dev_reactor_html_server 19091
/tmp/dev_reactor_html_client 127.0.0.1 19091 1 "GET /probe"
```

## 当前约束

- 事件模型：LT（水平触发）
- outputBuffer 溢出策略：默认丢弃新数据（可在 `EventLoop::Options` 中改）
- 单线程 Reactor
- 连接关闭语义：先从 EventLoop/Epoll 索引中摘除，连接对象析构时再最终 close fd

## 观测能力

`TcpConnection` 当前提供：

- `bytesRead`
- `bytesWritten`
- `framesDispatched`
- `droppedBytes`
- `protocolErrorCount`

`EventLoop` 当前聚合提供：

- `totalAccepted`
- `totalClosed`
- `activeConnections`
- `totalBytesRead`
- `totalBytesWritten`
- `totalFramesDispatched`
- `totalDroppedBytes`
- `totalProtocolErrors`

## 开发期策略测试

测试文件：`dev_policy_test.cpp`

编译：

```bash
g++ -std=c++17 -I/root/f-stack -I/root/f-stack/lib app/epoll/dev_policy_test.cpp -o /tmp/dev_policy_test
```

运行：

```bash
/tmp/dev_policy_test
```

覆盖点：

- Payload 超过 40960 字节会被拒绝并计数。
- outputBuffer 在 `kDropNewData` 策略下会记录丢弃字节数。
- dispatcher 接收到的 `payload` 包含 `MsgCode`（即 `FramePayload` 语义）。
