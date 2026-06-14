# 软件先行开发 Checklist 与计划

> **历史归档说明（2026-06-14）：** 本计划记录服务端快速验证阶段，旧 Python `device_gateway` 已移出本仓库。当前仓库只维护固件相关内容，服务端后续在 `/Users/jiangzhibin/workspace/chatbot-service-java` 开发。

> **状态（2026-06-13）：** 本计划列出的软件骨架已全部完成。完成后又新增了「云服务器 Docker 部署 + 管理后台安全加固 + 固件 `device_token` 支持」三项增量，见文末「计划完成后的增量」与 [`deployment.md`](deployment.md)。

## 目标

在 LMD2718 + NS4168 音频模块接入前，先完成硬件无关的软件骨架：

- 后台安全鉴权
- 连续对话会话管理
- 用户长期记忆配置
- 设备模拟器
- 语音接口占位协议
- 管理后台页面增强

今天不接硬件、不写 I2S 音频代码、不烧录设备。

## Checklist

- [x] 明确范围：今天只做软件，不接 LMD2718/NS4168，不写 I2S 音频代码，不烧录设备
- [x] 后台安全：管理端 token、设备端 token、prompt 长度限制、API Key 不返回前端/设备
  - ↳ 完成后升级：管理端单 token → **账号密码登录（HTTP Basic Auth）+ 随机路径**（详见文末增量）
- [x] 连续对话：`device_id`、`conversation_id`、最近 N 轮历史、创建新会话
- [x] 用户记忆：长期记忆继续由后台配置维护，不自动写入
- [x] 设备模拟器：Python CLI 模拟 ESP32 连续聊天
- [x] 语音接口预留：先定义 `/api/voice/chat`，返回 `voice_not_ready`
- [x] 管理后台：能配置 token、查看会话、测试指定设备/会话聊天
- [x] 测试：后端 unittest 覆盖鉴权、会话隔离、上下文拼接、模拟器核心逻辑
- [x] 验证：跑 unittest，打开后台页面检查，Arduino 编译确认现有固件不破

## 范围边界

### 今天做

- Python 后台配置模型扩展
- HTTP API 鉴权
- 会话历史存储
- 文本聊天连续对话
- 设备模拟器
- 语音接口占位
- 管理页面增强
- 标准库 `unittest` 测试

### 今天不做

- 不做真实 ASR
- 不做真实 TTS
- 不做 Opus/WebSocket 流式音频
- 不做小智完整协议
- 不做硬件 I2S 代码
- 不烧录 ESP32-S3

## 设计原则

- KISS：先用 HTTP + JSON，避免引入 WebSocket、MQTT、Opus 等复杂链路。
- YAGNI：真实语音链路等硬件接好后再做，现在只预留接口。
- DRY：聊天上下文拼接只放在一个服务层，HTTP 和设备模拟器共用同一协议。
- SOLID：配置、鉴权、会话、聊天、HTTP handler、模拟器拆成明确职责。

## 计划

### 1. 配置模型扩展

修改：

- `device_gateway/app_core.py`
- `tests/test_device_gateway_core.py`

在配置中增加：

- `admin_token`
- `device_token`
- `max_prompt_chars`
- `history_limit`

要求：

- `ConfigStore.get()` 能读取默认值和持久化值。
- `ConfigStore.update()` 只合并已知字段，忽略未知字段。
- `public_config()` 只返回 `admin_token_set`、`device_token_set`，不返回 token 明文。
- `api_key` 继续只返回 `api_key_set`。

验收：

```bash
python3 -m unittest /Users/jiangzhibin/Documents/ardiuno/tests/test_device_gateway_core.py
```

### 2. 鉴权层

修改：

- `device_gateway/server.py`
- `tests/test_device_gateway_http.py`

行为：

- `/admin`、`/api/config` 校验管理 token。
- `/api/chat` 校验 `X-Device-Token`。
- 未设置 token 时，允许本地开发默认通行，但响应中给出安全状态。
- 已设置 token 后：
  - 无 token 返回 `401`
  - token 错误返回 `403`
  - token 正确继续处理
- prompt 超过 `max_prompt_chars` 返回 `413` 或 `400`。

验收：

```bash
python3 -m unittest /Users/jiangzhibin/Documents/ardiuno/tests/test_device_gateway_http.py
```

### 3. 会话存储

新增：

- `device_gateway/conversation.py`
- `tests/test_conversation_store.py`

数据文件：

- `data/conversations.json`

数据结构：

```json
{
  "device_id": {
    "conversation_id": {
      "messages": [
        {"role": "user", "content": "..."},
        {"role": "assistant", "content": "..."}
      ],
      "created_at": "...",
      "updated_at": "..."
    }
  }
}
```

行为：

- `new_conversation(device_id)` 生成新的 `conversation_id`。
- `append_turn(device_id, conversation_id, prompt, answer)` 保存一轮用户和助手消息。
- `history(device_id, conversation_id, limit)` 返回最近 N 轮消息。
- 不同 `device_id` 的会话隔离。
- 同一 `device_id` 的不同 `conversation_id` 隔离。

验收：

```bash
python3 -m unittest /Users/jiangzhibin/Documents/ardiuno/tests/test_conversation_store.py
```

### 4. 聊天接口升级

修改：

- `device_gateway/app_core.py`
- `device_gateway/server.py`
- `tests/test_device_gateway_core.py`
- `tests/test_device_gateway_http.py`

请求从：

```json
{
  "prompt": "你好"
}
```

升级为：

```json
{
  "device_id": "esp32-dev-001",
  "conversation_id": "可选",
  "prompt": "你好"
}
```

返回：

```json
{
  "device_id": "esp32-dev-001",
  "conversation_id": "...",
  "answer": "..."
}
```

行为：

- 没传 `device_id` 时使用 `default-device`，兼容当前管理页测试。
- 没传 `conversation_id` 时后台自动创建。
- 调 DeepSeek 前拼接：
  - 全局提示词
  - 用户长期记忆
  - 最近 N 轮会话历史
  - 当前用户问题
- DeepSeek 返回后保存本轮对话。

验收：

```bash
python3 -m unittest discover -s /Users/jiangzhibin/Documents/ardiuno/tests -p "test_device_gateway*.py"
```

### 5. 新会话接口

修改：

- `device_gateway/server.py`
- `tests/test_device_gateway_http.py`

新增：

```text
POST /api/conversations/new
```

请求：

```json
{
  "device_id": "esp32-dev-001"
}
```

返回：

```json
{
  "device_id": "esp32-dev-001",
  "conversation_id": "..."
}
```

行为：

- 必须通过设备 token。
- 每次调用生成新的会话。
- 新会话不带旧会话历史。

### 6. 设备模拟器

新增：

- `device_gateway/device_simulator.py`
- `tests/test_device_simulator.py`

命令：

```bash
python3 -m device_gateway.device_simulator \
  --server http://127.0.0.1:8766 \
  --device-id esp32-dev-001 \
  --token xxx
```

交互命令：

- 普通输入：发送聊天
- `/new`：新会话
- `/exit`：退出

要求：

- 设备模拟器使用和 ESP32 一样的 HTTP 协议。
- 发送 `/api/chat` 时携带 `X-Device-Token`。
- 自动记住后台返回的 `conversation_id`。

### 7. 语音接口占位

修改：

- `device_gateway/server.py`
- `tests/test_device_gateway_http.py`

新增：

```text
POST /api/voice/chat
```

当前行为：

- 校验 `X-Device-Token`。
- 读取 `device_id`、`conversation_id`。
- 暂不处理音频。
- 返回：

```json
{
  "error": "voice_not_ready"
}
```

目的：

- 明天接硬件后不用重新设计协议入口。

### 8. 管理页面更新

修改：

- `device_gateway/server.py`

页面增加：

- 管理 token 输入
- 设备 token 输入
- `max_prompt_chars` 输入
- `history_limit` 输入
- 测试聊天时可填 `device_id`
- 显示返回的 `conversation_id`

注意：

- API Key、管理 token、设备 token 输入框留空时不修改旧值。
- 页面展示只显示“已配置/未配置”，不回显敏感值。

### 9. 最终验证

必须运行：

```bash
python3 -m unittest discover -s /Users/jiangzhibin/Documents/ardiuno/tests -p "test_*.py"
```

如果改动影响固件或公共协议，再运行 Arduino 编译：

```bash
ARDUINO_DIRECTORIES_DATA="/Users/jiangzhibin/Documents/ardiuno/.arduino-data" \
ARDUINO_DIRECTORIES_DOWNLOADS="/Users/jiangzhibin/Documents/ardiuno/.arduino-downloads" \
ARDUINO_DIRECTORIES_USER="/Users/jiangzhibin/Documents/ardiuno/.arduino-user" \
"/Users/jiangzhibin/Documents/ardiuno/.tools/arduino-cli" compile \
  --fqbn "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc" \
  --output-dir "/Users/jiangzhibin/Documents/ardiuno/build/esp32s3_wifi_provision" \
  "/Users/jiangzhibin/Documents/ardiuno/esp32s3_wifi_provision"
```

页面验证：

- 打开 `http://127.0.0.1:8766/admin`
- 保存配置
- 测试聊天
- 确认返回 `conversation_id`

## 完成标准

- 没有 token 的受保护请求被拒绝。
- token 正确时聊天可用。
- 同一 `conversation_id` 能带上历史上下文。
- 新会话不带旧上下文。
- 不同 `device_id` 的历史隔离。
- 管理页面不回显 API Key、管理 token、设备 token。
- 设备模拟器能连续聊天并创建新会话。
- `/api/voice/chat` 存在并返回 `voice_not_ready`。
- 全量 unittest 通过。

---

## 计划完成后的增量（2026-06-13）

本计划的软件骨架完成后，又做了三项工作。部署与安全加固属新增范围；固件仅改代码、**仍未烧录**，与原「今天不烧录」边界一致。完整部署与运维见 [`deployment.md`](deployment.md)。

### A. 云服务器 Docker 部署

- 后台从「本地 `python3 -m` 运行」升级为腾讯云服务器 Docker 容器常驻：`http://203.195.202.54:8766`
- 新增 `Dockerfile` / `docker-compose.yml` / `.dockerignore`（镜像 `python:3.12-slim`，无第三方依赖；`data/` 卷挂载持久化；`restart: unless-stopped` 崩溃/重启自愈）
- 腾讯云安全组放行 `TCP:8766`

### B. 管理后台安全加固

- 原「管理端单 token」升级为 **账号密码登录（HTTP Basic Auth）+ 随机路径**
- 配置项：`admin_token` → `admin_user` / `admin_password` / `admin_path_secret`
- 管理页地址：`/admin/<随机串>`；直接访问 `/admin` 或猜错随机串 → `404`（不暴露后台）
- `/api/config` 移至 `/admin/<随机串>/api/config`；设备接口（`/api/chat` 等）保持不变
- 改动 `app_core.py`、`server.py`，单元测试同步更新（22 项通过）

### C. 固件 device_token 支持（未烧录）

- `esp32s3_wifi_provision.ino`：新增 `device_token` 的 NVS 存取、`token` 串口命令、`ask` 时注入 `X-Device-Token`、`help` 更新
- `arduino-cli compile` 通过（占用 81% flash），按原边界**仍未烧录**

### 仍待办

- 登录管理页填写 DeepSeek **API Key**（未配置时 `/api/chat` 返回 500）
- 是否为 `/api/chat` 开启 `device_token`（需烧录已支持 token 的固件）
- （可选）后台启用 HTTPS，消除 Basic Auth 明文传输风险
