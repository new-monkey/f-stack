# F-Stack HTTP Reverse Proxy Example

## 概述 (Overview)

这个示例演示了如何使用 F-Stack 构建一个高性能的 HTTP 反向代理。程序从网卡接收 HTTP 请求，然后作为代理将请求转发到本地运行的真实 HTTP 服务（监听在 127.0.0.1），并将响应返回给客户端。

This example demonstrates how to build a high-performance HTTP reverse proxy using F-Stack. The program receives HTTP requests from the network card, acts as a proxy to forward requests to a real HTTP service running locally (listening on 127.0.0.1), and returns the response back to the client.

## 架构 (Architecture)

```
Internet Client
      ↓
  [Network Card]
      ↓
  F-Stack Proxy (port 80)
  - Receives packets via DPDK
  - Parses HTTP requests
      ↓
  Backend Server (127.0.0.1:8080)
  - Real HTTP service
  - Generates responses
      ↓
  F-Stack Proxy
  - Forwards response
      ↓
  Internet Client
```

## 主要特性 (Key Features)

- **零拷贝网络栈**: 使用 F-Stack/DPDK 实现内核旁路，获得超高网络性能
- **事件驱动架构**: 使用 epoll 实现高效的事件处理
- **全双工转发**: 同时处理客户端到后端和后端到客户端的数据流
- **连接池管理**: 高效管理客户端-后端连接对
- **Keep-Alive 支持**: 支持 HTTP 持久连接
- **非阻塞 I/O**: 所有套接字操作都是非阻塞的

## 文件说明 (Files)

- **main_proxy.c**: HTTP 反向代理主程序
- **backend_server.py**: 用于测试的简单 Python HTTP 后端服务器
- **Makefile**: 构建配置

## 构建 (Build)

### 前置条件 (Prerequisites)

1. 确保已经按照 F-Stack 主 README 完成了基础环境配置：
   - 安装 DPDK
   - 配置 hugepages
   - 绑定网卡到 DPDK
   - 编译并安装 F-Stack 库

2. 安装 Python 3（用于运行测试后端服务器）：
   ```bash
   sudo apt install python3  # Ubuntu/Debian
   # 或
   sudo yum install python3  # CentOS/RHEL
   ```

### 编译代理程序 (Compile Proxy)

```bash
cd /path/to/f-stack/example
make
```

这将生成三个可执行文件：
- `helloworld` - 原始的 kqueue 版本示例
- `helloworld_epoll` - 原始的 epoll 版本示例
- `helloworld_proxy` - **新的 HTTP 反向代理**

## 配置 (Configuration)

### 1. 配置后端端口 (Backend Port)

默认情况下，代理会连接到 `127.0.0.1:8080`。如果需要修改，编辑 `main_proxy.c`：

```c
#define BACKEND_PORT 8080  // 修改为你的后端服务端口
```

### 2. 配置 F-Stack

编辑 F-Stack 配置文件 `/etc/f-stack.conf` 或 `config.ini`，确保配置正确的网卡和 IP 地址：

```ini
[dpdk]
lcore_mask=1
channel=4
promiscuous=1
nb_ports=1
portmask=1
numa_on=1
tx_csum_offload_skip=0
tso=0
vlan_strip=1

[port0]
addr=192.168.1.2        # F-Stack 的 IP 地址
netmask=255.255.255.0
broadcast=192.168.1.255
gateway=192.168.1.1
```

## 使用方法 (Usage)

### 步骤 1: 启动后端服务器 (Start Backend Server)

在一个终端中启动测试后端服务器：

```bash
cd /path/to/f-stack/example
python3 backend_server.py
```

你应该看到：
```
Backend HTTP Server running on http://127.0.0.1:8080
This server will handle requests forwarded from the F-Stack proxy
Press Ctrl+C to stop
```

### 步骤 2: 启动 F-Stack 代理 (Start F-Stack Proxy)

在另一个终端中，以 root 权限启动代理：

```bash
cd /path/to/f-stack/example
sudo ./helloworld_proxy -c /etc/f-stack.conf
```

代理将开始监听网卡上的 80 端口。

### 步骤 3: 测试 (Test)

从另一台机器或使用配置的 IP 地址测试：

```bash
# 使用 curl
curl http://192.168.1.2/

# 使用浏览器
# 打开 http://192.168.1.2/
```

你应该看到由后端服务器生成的 HTML 页面，确认代理工作正常。

## 代理工作流程 (Proxy Workflow)

1. **接收客户端连接**: F-Stack 代理在网卡上接收新的 TCP 连接
2. **读取 HTTP 请求**: 读取并缓冲完整的 HTTP 请求（包括头部和正文）
3. **连接后端服务器**: 建立到 127.0.0.1:8080 的新连接
4. **转发请求**: 将完整的 HTTP 请求转发到后端服务器
5. **读取后端响应**: 从后端服务器读取 HTTP 响应
6. **转发响应**: 将响应转发回客户端
7. **连接管理**: 支持 Keep-Alive，可重用连接处理多个请求

## 连接状态机 (Connection State Machine)

```
IDLE
  ↓
READING_REQUEST (从客户端读取 HTTP 请求)
  ↓
CONNECTING_BACKEND (连接到后端服务器)
  ↓
SENDING_REQUEST (向后端发送请求)
  ↓
READING_RESPONSE (从后端读取响应)
  ↓
SENDING_RESPONSE (向客户端发送响应)
  ↓
回到 READING_REQUEST (Keep-Alive) 或 关闭连接
```

## 数据结构 (Data Structures)

### connection_pair_t

每个活动的代理会话都有一个连接对结构：

```c
typedef struct connection_pair {
    int client_fd;              // 客户端套接字
    int backend_fd;             // 后端套接字
    conn_state_t state;         // 连接状态
    
    char request_buf[8192];     // 请求缓冲区
    int request_len;            // 已接收请求长度
    int request_sent;           // 已发送请求长度
    
    char response_buf[8192];    // 响应缓冲区
    int response_len;           // 已接收响应长度
    int response_sent;          // 已发送响应长度
    
    int active;                 // 连接是否活动
} connection_pair_t;
```

## 性能优化建议 (Performance Tuning)

1. **增加连接池大小**: 修改 `MAX_CONNECTIONS` 以支持更多并发连接
2. **调整缓冲区大小**: 修改 `BUFFER_SIZE` 以处理更大的请求/响应
3. **使用多进程**: 利用 F-Stack 的多进程架构扩展到多个 CPU 核心
4. **后端连接复用**: 实现连接池复用后端连接，减少连接建立开销
5. **零拷贝优化**: 考虑使用 F-Stack 的零拷贝 API（参考 main_zc.c）

## 日志和调试 (Logging and Debugging)

程序使用 F-Stack 的日志系统输出调试信息。要查看详细日志，可以修改 F-Stack 配置文件中的日志级别：

```ini
[log]
level=DEBUG  # 可选: ERR, WARNING, INFO, DEBUG
```

## 已知限制 (Known Limitations)

1. **简单的 HTTP 解析**: 当前实现使用简单的字符串搜索来检测完整请求，不是完整的 HTTP 解析器
2. **缓冲区限制**: 请求和响应大小受 `BUFFER_SIZE` 限制（默认 8KB）
3. **单个后端**: 当前只支持单个后端服务器，未实现负载均衡
4. **无 HTTPS**: 不支持 SSL/TLS 终止

## 扩展建议 (Future Enhancements)

1. **完整的 HTTP 解析器**: 集成 http-parser 库进行正确的 HTTP 协议处理
2. **后端负载均衡**: 支持多个后端服务器，实现轮询或加权负载均衡
3. **健康检查**: 定期检查后端服务器健康状态
4. **连接池**: 维护到后端的持久连接池
5. **SSL/TLS 支持**: 添加 OpenSSL 集成以支持 HTTPS
6. **请求修改**: 添加/删除 HTTP 头部，URL 重写
7. **缓存**: 实现 HTTP 响应缓存
8. **监控和统计**: 添加请求计数、延迟监控等

## 故障排除 (Troubleshooting)

### 问题：代理无法启动
- 检查是否有足够权限（需要 root）
- 确认 F-Stack 配置文件路径正确
- 验证网卡已正确绑定到 DPDK

### 问题：无法连接到后端
- 确认后端服务器正在运行并监听 127.0.0.1:8080
- 检查防火墙规则
- 验证 `BACKEND_PORT` 设置正确

### 问题：性能不佳
- 检查 CPU 亲和性配置
- 验证 DPDK 配置（hugepages、CPU 核心等）
- 考虑增加 `MAX_CONNECTIONS` 和 `BUFFER_SIZE`

## 参考资料 (References)

- [F-Stack 主文档](https://github.com/F-Stack/f-stack)
- [DPDK 文档](https://doc.dpdk.org/)
- [HTTP/1.1 规范](https://tools.ietf.org/html/rfc7230)

## 许可证 (License)

与 F-Stack 项目相同的许可证。详见主项目的 LICENSE 文件。

## 作者 (Author)

基于 F-Stack 示例代码开发的 HTTP 反向代理实现。
