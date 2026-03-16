通信协议编码说明：

术语规范：详见 `TERMINOLOGY.md`。本文中的 Frame、FrameLen、FramePayload、MsgHeader、MsgBody 均遵循该规范。

1. 传输帧由 `FrameLen(4B, 网络序)` 和 `FramePayload` 组成。
2. `FrameLen` 表示 `FramePayload` 的字节数，不包含前 4 字节 `FrameLen` 本身。
3. `FramePayload` 以 `MsgCode(4B, 网络序)` 作为第一个字段，后续是可扩展业务头字段和消息体。
4. `FramePayload` 最大为 40960 字节。

```
Frame := FrameLen(4B) + FramePayload
FramePayload := MsgHeader + MsgBody

MsgHeader := MsgCode(4B) + MsgVersion(4B) + MsgLen(4B) + <Other Fields>...
MsgBody := [] or byte[MsgLen-sizeof(MsgHeader)]

MsgLen == sizeof(MsgHeader) + sizeof(MsgBody)
FrameLen == sizeof(FramePayload) == MsgLen
sizeof(FramePayload) <= 40960
sizeof(Frame) = 4 + FrameLen
```

术语澄清：

1. `FrameLen` 指协议字段值（仅 `FramePayload` 长度）。
2. 代码中的整帧字节数建议命名为 `frameBytes` 或 `totalFrameBytes`，其值为 `4 + FrameLen`。

实现约定（当前 Epoll 组件）：

1. 解析阶段先读取 `FrameLen` 和 `MsgCode`，并将回调 `payload` 视图定义为 `FramePayload`（即从 `MsgCode` 开始，长度为 `FrameLen`）。
2. 业务回调可在 `payload` 内继续解析 `MsgVersion/MsgLen/<Other Fields>`，并自行做完整一致性校验。

统一编解码器（`FrameCodec`）约定：

1. `FrameCodec::encode(msgCode, body)`：统一进行传输层编码，输出 `FrameLen + MsgCode + Body`。
2. `FrameCodec::encode(payload)`：当业务层已完成 `FramePayload`（已包含 `MsgCode`）编码时，直接封装为 `FrameLen + FramePayload`。
3. `FrameCodec::decodeHeader(...)`：统一解码固定 8 字节头，得到 `framePayloadLen` 与 `msgCode`。
4. `FrameCodec::isFramePayloadLenValid(...)`：统一检查最小载荷与最大载荷。
5. `FrameCodec::totalFrameBytes(framePayloadLen)`：统一计算整帧字节数（`4 + framePayloadLen`）。
6. `FrameCodec::payloadFromFrame(...)` / `FrameCodec::extractMsgCode(...)` / `FrameCodec::bodyFromPayload(...)`：统一对 `FramePayload` 做拆分。