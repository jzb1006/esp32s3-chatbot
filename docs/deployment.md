# ESP32-S3 LLM 后台 — 部署与变更文档

| 版本 | 日期 | 变更摘要 |
|------|------|---------|
| v1 | 2026-06-13 | 初版：`llm_admin` 后台 Docker 化部署到腾讯云服务器；管理后台安全加固（账号密码登录 + 随机路径）；固件新增 `device_token` 支持 |

---

## 1. 概述

本次工作把 `llm_admin` 后台从「本地 `python3 -m` 运行」升级为「云服务器 Docker 常驻部署」，并在此基础上做了两项增强：

1. **管理后台安全加固** —— 原 `/admin` 公网裸奔，改为「账号密码登录（HTTP Basic Auth）+ 不可猜的随机路径」。
2. **固件 `device_token` 支持** —— ESP32 固件 `ask` 命令支持携带 `X-Device-Token`，为后续给 `/api/chat` 开启设备鉴权做准备（代码已就绪，**未烧录**）。

---

## 2. 部署架构

| 项 | 值 |
|---|---|
| 云厂商 / 系统 | 腾讯云 CVM · Ubuntu 24.04 LTS · x86_64 · 4 核 / 3.6G |
| 公网 IP | `203.195.202.54` |
| 后台访问地址 | `http://203.195.202.54:8766` |
| 部署方式 | Docker Compose（镜像 `python:3.12-slim`，无第三方依赖）|
| 服务器部署目录 | `/opt/llm_admin` |
| 容器名 / 重启策略 | `llm_admin` / `restart: unless-stopped`（崩溃 / 重启自愈）|
| 端口映射 | 宿主 `8766` → 容器 `8766`（腾讯云安全组已放行入站 TCP:8766）|
| 数据持久化 | 宿主 `/opt/llm_admin/data` 挂载到容器 `/app/data`（`llm_config.json`、`conversations.json`）|
| SSH 登录 | `root@203.195.202.54`，私钥 `~/.ssh/chatbot.pem`（腾讯云密钥对名 `chatbot`）|

> **为什么用 Docker：** 服务器宿主自带 Python 3.6.8，而后台代码用了 `str.removeprefix()` 等 Python 3.9+ 语法，宿主直接跑会崩。容器内用 Python 3.12 隔离版本，代码零改动。

---

## 3. 本次新增 / 修改的文件清单

### 新增（本地项目根）

| 文件 | 作用 |
|---|---|
| `Dockerfile` | 镜像定义：`python:3.12-slim` + `PYTHONUNBUFFERED=1`（日志实时进 `docker logs`），只 `COPY llm_admin/`，`data/` 走卷挂载 |
| `docker-compose.yml` | 编排：端口 `8766`、`data` 卷挂载、`restart: unless-stopped` |
| `.dockerignore` | 只把 `llm_admin` 包打进镜像，排除其余 |
| `docs/deployment.md` | 本文档 |

### 修改

| 文件 | 改动 |
|---|---|
| `llm_admin/app_core.py` | 配置项：移除 `admin_token`，新增 `admin_user` / `admin_password` / `admin_path_secret`；`public_config()` 同步调整（只回显 `*_set` 布尔与账号名，不回显密码 / 路径串明文）|
| `llm_admin/server.py` | 鉴权改 HTTP Basic Auth；管理页路径随机化；`/api/config` 移到随机路径前缀下；管理页 JS 适配 |
| `esp32s3_wifi_provision/esp32s3_wifi_provision.ino` | 新增 `device_token` 的 NVS 存取、`token` 串口命令、`ask` 时注入 `X-Device-Token` header、`help` 更新 |
| `tests/test_llm_admin_core.py`、`tests/test_llm_admin_http.py` | 同步更新鉴权 / 配置项相关用例 |

> 全量单元测试 22 项通过；固件 `arduino-cli compile` 通过（占用 81% flash）。

---

## 4. 部署步骤（可复现）

以下命令在**本地项目根** `/Users/jiangzhibin/Documents/ardiuno` 执行。

```bash
KEY=~/.ssh/chatbot.pem

# 1. 同步代码 + 部署文件到服务器
ssh -i "$KEY" root@203.195.202.54 'mkdir -p /opt/llm_admin'
rsync -az --delete -e "ssh -i $KEY" --exclude='__pycache__' --exclude='*.pyc' \
  llm_admin/ root@203.195.202.54:/opt/llm_admin/llm_admin/
rsync -az -e "ssh -i $KEY" \
  Dockerfile docker-compose.yml .dockerignore root@203.195.202.54:/opt/llm_admin/

# 2. 构建并启动容器
ssh -i "$KEY" root@203.195.202.54 'cd /opt/llm_admin && docker compose up -d --build'
```

**安全组**：在腾讯云控制台给该实例绑定的安全组添加入站规则 —— 来源 `0.0.0.0/0`、协议端口 `TCP:8766`、策略「允许」。

---

## 5. 运维命令

```bash
KEY=~/.ssh/chatbot.pem
HOST=root@203.195.202.54

# 查看容器状态
ssh -i "$KEY" $HOST 'docker ps --filter name=llm_admin'

# 查看日志（启动日志 / 错误）
ssh -i "$KEY" $HOST 'docker logs --tail=50 llm_admin'

# 重启 / 停止 / 启动
ssh -i "$KEY" $HOST 'cd /opt/llm_admin && docker compose restart'
ssh -i "$KEY" $HOST 'cd /opt/llm_admin && docker compose down'
ssh -i "$KEY" $HOST 'cd /opt/llm_admin && docker compose up -d'

# 更新代码后重新部署（本地执行 rsync 后）
ssh -i "$KEY" $HOST 'cd /opt/llm_admin && docker compose up -d --build'

# 备份配置与会话数据
ssh -i "$KEY" $HOST 'cat /opt/llm_admin/data/llm_config.json'
```

---

## 6. 管理后台安全加固（重点）

### 6.1 背景

加固前，`/admin` 与 `/api/config` 在 `admin_token` 为空时对公网直接放行（裸奔）：任何人知道 IP+端口即可打开管理页、修改配置。

### 6.2 改动设计

- **随机路径**：管理页地址变为 `http://203.195.202.54:8766/admin/<admin_path_secret>`，`admin_path_secret` 为部署时随机生成的 24 位串。直接访问 `/admin` 或猜错随机串 → **404**（伪装成不存在，不暴露后台位置）。
- **账号密码登录**：访问正确地址时返回 `401 + WWW-Authenticate: Basic`，浏览器弹出账号密码框；校验 `admin_user` / `admin_password`（HTTP Basic Auth）通过才放行。
- **配置接口同前缀**：`/api/config` 移到 `/admin/<secret>/api/config`，浏览器登录后对该前缀自动携带 Basic 凭据；管理页 JS 用 `location.pathname + '/api/config'` 动态拼接，不硬编码 secret。
- **设备接口不变**：`/api/chat`、`/api/conversations/new`、`/api/voice/chat` 路径与 `X-Device-Token` 鉴权方式均**保持不变**。

### 6.3 配置项变化

| 加固前 | 加固后 |
|---|---|
| `admin_token`（单一令牌）| `admin_user`（账号）+ `admin_password`（密码）+ `admin_path_secret`（路径随机串）|

### 6.4 管理凭据

- 账号 / 密码 / 路径串均在**部署时随机生成**，写入服务器 `/opt/llm_admin/data/llm_config.json`，并已在交付时单独告知（**本文档不记录明文**）。
- 登录管理页后可在页面自行修改账号 / 密码 / 路径串（留空表示不修改）。改路径串后需用新地址 `/admin/<新串>` 重新登录。

### 6.5 忘记凭据时如何重置

通过 SSH 在服务器上重新生成（容器会实时读取配置文件，无需重启）：

```bash
ssh -i ~/.ssh/chatbot.pem root@203.195.202.54 'python3 - <<PY
import json, secrets, string
from pathlib import Path
p = Path("/opt/llm_admin/data/llm_config.json")
cfg = json.loads(p.read_text()) if p.exists() else {}
cfg["admin_user"] = "adm_" + secrets.token_hex(4)
cfg["admin_password"] = secrets.token_urlsafe(18)
cfg["admin_path_secret"] = "".join(secrets.choice(string.ascii_lowercase+string.digits) for _ in range(24))
p.write_text(json.dumps(cfg, ensure_ascii=False, indent=2))
print("user=", cfg["admin_user"]); print("pass=", cfg["admin_password"]); print("secret=", cfg["admin_path_secret"])
PY'
```

---

## 7. 固件 `device_token` 支持

### 7.1 改动（`esp32s3_wifi_provision.ino`，4 处）

1. 新增 `loadDeviceToken()` / `saveDeviceToken()`（仿 `loadAdminUrl`，存 NVS namespace `wifi` 的 key `device_token`）。
2. `askAdminBackend()` 在 token 非空时 `http.addHeader("X-Device-Token", token)`。
3. 新增串口命令 `token <device-token>`（传空值则清除）。
4. `help` 输出新增 token 命令说明，并显示当前 token `set` / `unset`。

### 7.2 串口命令用法

```text
help                      # 查看命令与当前状态
admin http://203.195.202.54:8766   # 设置后台地址（注意：公网用服务器 IP）
token <设备Token>          # 设置设备 token（需与后台 device_token 一致）
ask <prompt>              # 提问，自动携带 X-Device-Token
```

### 7.3 状态

代码已就绪，`arduino-cli compile` 通过（占用 81% flash）。**当前未烧录** —— 按计划先用后台「测试聊天」/ `device_simulator.py` 在本地跑通，确认后再烧录。

烧录命令（接设备后执行）：

```bash
arduino-cli upload -p /dev/cu.usbmodem101 \
  --fqbn "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc" \
  --input-dir build/esp32s3_wifi_provision esp32s3_wifi_provision
```

---

## 8. 接口契约变更摘要

| 端点 | 加固前 | 加固后 |
|---|---|---|
| 管理页 | `GET /admin` | `GET /admin/<secret>`（Basic Auth）|
| 配置读 / 写 | `GET/POST /api/config`（`X-Admin-Token`）| `GET/POST /admin/<secret>/api/config`（Basic Auth）|
| 聊天 | `POST /api/chat`（`X-Device-Token`）| **不变** |
| 新会话 | `POST /api/conversations/new`（`X-Device-Token`）| **不变** |
| 语音占位 | `POST /api/voice/chat` → `voice_not_ready` | **不变** |

---

## 9. 已知风险与安全 Review 点

1. **🔴 HTTP 明文传输**：当前为 HTTP，Basic Auth 的账号密码以 base64（非加密）传输，公网下存在被中间人嗅探的理论风险。比裸奔安全得多，但**彻底安全需上 HTTPS**（域名 + Let's Encrypt，或 nginx / caddy 反代）。
2. **`admin_password` 为空时放行**：仅用于首次初始化。**勿在管理页清空管理密码**，否则退回裸奔状态。
3. **路径串出现在 URL**：随机路径会出现在浏览器历史等；它只是「第一层隐蔽」，真正的访问控制是账号密码。后台已关闭 access log。
4. Basic Auth 使用 `hmac.compare_digest` 常量时间比较，账号与密码两项均参与比较，规避时序侧信道。
5. `public_config` 不回显密码 / 路径串明文，仅返回 `*_set` 布尔与账号名。

---

## 10. 待办（TODO）

- [ ] 登录管理页填写 **DeepSeek API Key**（未配置时 `/api/chat` 返回 500）。
- [ ] 决定是否为 `/api/chat` 开启 `device_token`（开启后需用已支持 token 的固件，重新烧录）。
- [ ] （可选）为后台启用 **HTTPS**，消除 Basic Auth 明文传输风险。
- [ ] （可选）固件烧录最新版（含 `device_token` 支持）。
