# app/epoll 术语规范

本规范用于统一传输层与业务层的命名，降低维护时的语义歧义。

## 分层模型

1. 传输层（Transport Layer）
2. 业务层（Application Layer）

传输层只关心帧边界与基础字段（FrameLen、MsgCode）；业务层负责 MsgHeader 其他字段与 MsgBody 解析。

## 术语字典

1. Frame
含义：一个完整传输单元。
结构：FrameLen(4B) + FramePayload。

2. FrameLen
含义：FramePayload 的字节数。
约束：不包含自身 4B 长度字段。

3. FramePayload
含义：Frame 中除 FrameLen 外的全部内容。
结构：MsgHeader + MsgBody。
约束：首字段必须是 MsgCode(4B)。

4. MsgCode
含义：业务消息类型。
位置：FramePayload 前 4 字节。

5. MsgHeader
含义：业务头。
结构：至少包含 MsgCode，可扩展 MsgVersion、MsgLen、其他字段。

6. MsgBody
含义：业务消息体。

## 代码命名规范

1. 长度字段命名
使用 framePayloadLen，不使用 payloadLen 这种泛化命名。

2. 视图命名
使用 framePayload 表示从 MsgCode 开始的视图。
使用 body 表示去掉 MsgCode 后的业务体。

3. 网络序变量命名
使用 netFrameLen、netMsgCode。

4. 回调参数命名
MessageDispatcher 回调第二个参数命名为 framePayload。

## 禁用同义词

1. payload（未注明层次）
原因：可能表示 FramePayload，也可能表示 MsgBody。

2. header（未注明层次）
原因：可能表示传输层 8B 头，也可能表示业务头。

## 推荐表达

1. 传输层固定头：Frame Header（FrameLen + MsgCode）
2. 分发入参：FramePayload
3. 业务解析结果：MsgHeader 与 MsgBody

## 与现有接口映射

1. FrameCodec::encode(msgCode, body)
输入：MsgCode + MsgBody。
输出：Frame。

2. FrameCodec::encode(framePayload)
输入：已编码 FramePayload（首字段是 MsgCode）。
输出：Frame。

3. TcpConnection::parseFrames
分发参数：framePayload（从 MsgCode 开始，长度等于 FrameLen）。