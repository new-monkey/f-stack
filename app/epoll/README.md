# app/epoll 开发说明

本目录提供 Reactor 组件的基础实现，支持两种后端：

- Kernel 后端：使用 `epoll_*` + `read/write`，用于开发阶段替代测试。
- F-Stack 后端：使用 `ff_epoll_*` + `ff_read/ff_write`，用于最终集成。

## 后端切换

通过 `EpollBackendType` 在运行期选择后端：

- `EpollBackendType::kKernel`
- `EpollBackendType::kFStack`

同时，F-Stack 代码路径默认是编译期可选的：

- 若定义 `FSTACK_ENABLE_BACKEND`，会启用真实 `ff_*` 调用。
- 若不定义该宏，`kFStack` 分支返回 `ENOSYS`，可保证 kernel-only 链接通过。

## 开发期最小可运行示例

示例文件：`dev_kernel_echo_server.cpp`

- 使用 `Acceptor` 组件负责 listen/accept 与非阻塞设置。
- 使用 `EventLoop` + `TcpConnection` 处理读写与消息分发。

编译：

```bash
g++ -std=c++17 -I/root/f-stack -I/root/f-stack/lib app/epoll/dev_kernel_echo_server.cpp -o /tmp/dev_kernel_echo_server
```

运行：

```bash
/tmp/dev_kernel_echo_server 19090
```

协议格式：

- Header: `Length(4 bytes, network order) + MsgCode(4 bytes, network order)`
- Payload: 长度上限 `40960` 字节

默认注册：

- `MsgCode == 1`：回显 payload。

## 当前约束

- 事件模型：LT（水平触发）
- outputBuffer 溢出策略：默认丢弃新数据（可在 `EventLoop::Options` 中改）
- 单线程 Reactor

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
