# 项目状态与后续计划 —— ESP32 语音助手 × Hermes Agent

| 版本 | 日期 | 说明 |
|------|------|------|
| v1 | 2026-06-14 | 初版：进度总账 + 待办清单，供后期继续 |

> 进度总账 + 待办。详细决策见 [`adr/ADR-001-session-state-ownership.md`](./adr/ADR-001-session-state-ownership.md)、实现方案见 [`hermes-agent-integration-B-plan.md`](./hermes-agent-integration-B-plan.md)、部署见 [`deployment.md`](./deployment.md) §11。

---

## 一、已完成 ✅

### 1. 方案与文档
- **B-plan 评审**：发现 v1 选的 B-1（`chat/completions` + session 头让服务端记历史）假设被 Hermes 官方文档证伪（该端点无状态），改用 `/v1/responses`。
- **B-plan 升 v3**：选定 **B-2b 命名会话**（`conversation_id` 透传）、移除 off/DeepSeek 回退、中间件瘦身、改名。
- **ADR-001**（Accepted）：会话/记忆归属迁移到 Hermes + 中间件瘦为设备网关 + 改名 `device_gateway` 的决策记录。
- **deployment.md** §11/§11.5：架构变更说明 + 实际部署记录。
- **hermes-spike.sh**：部署后验证脚本（命名会话/失效/output 结构）。
- design / README / checklist 路径引用更新。

### 2. 中间件代码（`llm_admin` → `device_gateway`）
- **改名**：包目录 / import / Dockerfile / compose / 文案全部更新。
- **瘦身**：删 DeepSeek 直连回退、`ConversationStore` 整模块、`build_messages`、`system_prompt`/`user_memory`/`history_limit`/`hermes_session_mode` 配置。
- **纯网关**：`POST /v1/responses` + `conversation=conversation_id` 透传 + `X-Hermes-Session-Key=owner` + 遍历 `output[]` 取 `output_text`；`conversation_id` 改 `uuid` 生成、零本地会话状态。
- 管理页瘦身；**单元测试重写 19 项通过**；对设备 `/api/chat` 契约不变（固件不烧录）。

### 3. 服务器部署（`203.195.202.54`，已上线）
- **hermes**：daocloud 代理拉镜像(3.36GB) + DeepSeek provider/key + `model=deepseek/deepseek-chat` + `API_SERVER_*`(.env) + 减重(`environment_probe=false`) + **安全收紧(禁 10 个危险工具)** + gateway 容器(hermes-net，`:8642` 仅 127.0.0.1)。
- **device_gateway**：迁移 `/opt/llm_admin`→`/opt/device_gateway`，build + 起(接 hermes-net)，配置对接 hermes。
- **验证**：spike S-A 命名会话多轮通过；端到端「设备→网关→hermes→DeepSeek」通 + 多轮记忆生效（热响应 ~1s）。

---

## 二、未完成 / 后期继续 ⏳

### A. 安全收尾（短期，建议优先）
- [ ] **DeepSeek key 轮换** —— 部署时在对话里暴露过，控制台换新；换完更新服务器 `~/.hermes/.env`(`DEEPSEEK_API_KEY`) + `/opt/device_gateway/data/llm_config.json`(`api_key`)。
- [ ] **device_token** —— `8766` 公网当前无设备鉴权（terminal 已禁、风险已大降但非零）；配了需固件烧录对应 token，要协调。
- [ ] **HTTPS** —— HTTP 明文，建议域名 + Let's Encrypt 或 nginx/caddy 反代。

### B. 代码提交
- [ ] **本地改动未 git commit**（瘦身 + 改名 + 文档 + ADR + spike + compose 加网络）。改名是 `mv` 做的，commit 前 `git add -A` 让 git 识别 rename。

### C. 语音链路（中长期 · `design` §14 roadmap）
当前处于**阶段一（文本对话 + Hermes 记忆）= 已完成**。语音 6 层里只做了第 4 层（云端 LLM 大脑）。
- [ ] **阶段二（流式）**：device_gateway 加 SSE 透传（如 `/api/chat/stream`），向 hermes 发 `stream:true` 边收边转；设备/模拟器读流。**纯云端、模拟器可验证、不依赖硬件**。
- [ ] **阶段三（语音）**：
  - [ ] 音频硬件：I2S 麦克风 INMP441 + I2S 功放 MAX98357A + 喇叭。
  - [ ] 唤醒词：ESP-SR WakeNet（需 **Arduino → ESP-IDF 框架迁移**，或 TFLite Micro 自训）；先用内置词如 "Hi ESP"。
  - [ ] 云 ASR 选型 + 接入（Whisper / 腾讯云 / 讯飞 …）。
  - [ ] 云 TTS 选型 + 接入（流式输出）。
  - [ ] 打通 device_gateway `/api/voice/chat`（当前返回 `voice_not_ready` 占位）。

### D. 其他遗留
- [ ] 固件 `.ino` 里 "LLM"/"管理后台" 文案（不影响功能，下次烧录顺手改）。
- B-2a（`previous_response_id` 链）回退预案：spike 已确认 B-2b 成立，B-2a 仅作 B-plan 文档备选，**无需实现**。

---

## 三、关键信息速查

| 项 | 值 |
|---|---|
| 服务器 | `root@203.195.202.54`，证书 `~/.ssh/chatbot.pem` |
| hermes | 数据 `/root/.hermes`（config.yaml/.env/state）；容器 `hermes`；`:8642` 仅 `127.0.0.1` + hermes-net |
| device_gateway | `/opt/device_gateway`；容器 `device_gateway`；`:8766` 公网（安全组已放行）|
| 镜像拉取 | 走 daocloud 代理 `docker.m.daocloud.io/...`（Docker Hub 国内不通），retag 回官方名 |
| 底层模型 | DeepSeek（`deepseek-chat` → 实际 `deepseek-v4-flash`）|
| 部署后验证 | `HERMES_URL=http://127.0.0.1:8642/v1 API_KEY=<key> bash docs/hermes-spike.sh` |
| 管理后台 | `http://203.195.202.54:8766/admin/<随机串>`（Basic Auth，凭据已迁移保留）|
</content>
