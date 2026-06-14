# 服务端部署说明

| 版本 | 日期 | 变更摘要 |
|------|------|---------|
| v1 | 2026-06-13 | Python `device_gateway` Docker 部署记录 |
| v2 | 2026-06-14 | Hermes 接入与设备网关瘦身 |
| v3 | 2026-06-14 | 服务端迁移到 `chatbot-service-java`，本仓库只保留固件 |

---

## 当前边界

本仓库 `/Users/jiangzhibin/Documents/ardiuno` 现在只维护 ESP32-S3 固件、固件测试和硬件相关文档。

旧 Python `device_gateway`、Dockerfile、Compose、服务端测试和设备模拟器已经从本仓库移除。服务端能力迁移到：

```text
/Users/jiangzhibin/workspace/chatbot-service-java
```

后续服务端部署、Hermes 配置、设备网关鉴权和小智语音网关都应在 Java 服务端仓库维护。

---

## 固件依赖的服务端契约

`esp32s3_wifi_provision` 固件只依赖 HTTP 契约，不依赖服务端实现语言。

| 接口 | 方法 | 固件用途 |
|---|---|---|
| `/api/chat` | `POST` | 串口 `ask <prompt>` 普通文本聊天 |
| `/api/chat/stream` | `POST` | 串口 `askstream <prompt>` 流式文本聊天 |
| `/api/conversations/new` | `POST` | 新建连续对话 ID |

请求约定：

- `Content-Type: application/json`
- 已配置设备 Token 时携带 `X-Device-Token`
- 请求体使用 `prompt`，可选 `device_id`、`conversation_id`
- `/api/chat` 响应 JSON 需包含 `answer`
- `/api/chat/stream` 响应 `text/event-stream`，固件打印 `response.output_text.delta` 事件里的 `delta`

---

## 固件配置服务端地址

打开串口：

```bash
arduino-cli monitor -p /dev/cu.usbmodem101 -c baudrate=115200
```

配置服务端：

```text
admin http://<服务端地址>:8766
token <设备Token>
```

验证：

```text
ask 请只回复：pong
askstream 请只回复：pong
```

`askstream` 依赖 Java 服务端补齐 `/api/chat/stream`。如果服务端尚未实现，固件会打印对应 HTTP 错误。

---

## 历史说明

2026-06-13 至 2026-06-14 期间，本仓库曾包含 Python 标准库实现的 `device_gateway`，用于快速验证 ESP32-S3 到 Hermes Agent 的文本链路。

该实现已经由 `/Users/jiangzhibin/workspace/chatbot-service-java` 接管。历史部署命令、Python Docker 构建方式和管理后台实现不再作为当前操作手册维护；如需追溯，请查看 Git 历史和：

- [`adr/ADR-001-session-state-ownership.md`](./adr/ADR-001-session-state-ownership.md)
- [`hermes-agent-integration-B-plan.md`](./hermes-agent-integration-B-plan.md)
