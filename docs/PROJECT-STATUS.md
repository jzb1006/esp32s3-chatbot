# 项目状态与后续计划 —— ESP32-S3 固件

| 版本 | 日期 | 说明 |
|------|------|------|
| v1 | 2026-06-14 | 初版：ESP32-S3 + Hermes Agent 软件优先链路 |
| v2 | 2026-06-14 | 新增固件端 `askstream` 流式命令 |
| v3 | 2026-06-14 | 确认 16MB Flash，并切换到 16M/3MB APP 分区 |
| v4 | 2026-06-14 | 服务端迁移到 `chatbot-service-java`；本仓库只保留固件相关内容 |

---

## 当前边界

本仓库 `/Users/jiangzhibin/Documents/ardiuno` 只维护：

- ESP32-S3 Arduino 固件源码
- 固件相关单元测试
- 烧录、串口、硬件和固件接口文档

服务端设备网关已经迁移到：

```text
/Users/jiangzhibin/workspace/chatbot-service-java
```

旧 Python `device_gateway`、Dockerfile、Compose、设备模拟器和服务端测试已经从本仓库移除。后续服务端部署、Hermes 配置、设备鉴权、小智语音网关和 `/api/chat/stream` 补齐都在 Java 服务端仓库处理。

---

## 已完成

### 1. 固件

- 当前主固件：`esp32s3_wifi_provision`
- 当前 FQBN：`esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB`
- 串口：`/dev/cu.usbmodem101`
- 固件命令：`help`、`admin`、`token`、`ask`、`askstream`
- `ask` 调用服务端 `POST /api/chat`
- `askstream` 调用服务端 `POST /api/chat/stream`
- 已确认 ESP32-S3 Flash 16MB、PSRAM 8MB；当前 APP 分区 3MB

### 2. 服务端迁移

- Python `device_gateway` 已由 Java 服务端接管。
- 固件仓库不再保存 API Key、Hermes Key、服务端管理凭据或服务端部署配置。
- 固件只保存服务端 URL 和可选 `device_token`。

### 3. 历史决策

- ADR-001 记录了会话 / 记忆归属迁移到 Hermes 的决策。
- `hermes-agent-integration-B-plan.md` 记录了 Python 中间件阶段的设计与落地过程，作为历史资料保留。
- `deployment.md` 已改为当前迁移后边界说明。

---

## 待办

- [ ] Java 服务端补齐 `/api/chat/stream`，以恢复固件 `askstream` 的完整后端能力。
- [ ] 根据 Java 服务端实际部署地址，重新通过串口执行 `admin http://<服务端地址>:8766`。
- [ ] 如服务端启用设备鉴权，通过串口执行 `token <设备Token>`。
- [ ] 固件 `.ino` 里仍有 `LLM`、`admin backend` 等旧文案，可在下次固件改动时顺手改成“服务端”。
- [ ] 后续接入音频硬件后，先做 I2S 麦克风 / 功放自检，再扩展语音链路。

---

## 关键信息速查

| 项 | 值 |
|---|---|
| 固件仓库 | `/Users/jiangzhibin/Documents/ardiuno` |
| 服务端仓库 | `/Users/jiangzhibin/workspace/chatbot-service-java` |
| 主固件 | `esp32s3_wifi_provision` |
| 串口 | `/dev/cu.usbmodem101` |
| USB | ESP32-S3 原生 USB-Serial-JTAG，需 `CDCOnBoot=cdc` |
| Flash / PSRAM | 16MB / 8MB |
| 当前编译分区 | `FlashSize=16M,PartitionScheme=app3M_fat9M_16MB` |
| 固件 HTTP 契约 | `/api/chat`、`/api/chat/stream`、`/api/conversations/new` |
