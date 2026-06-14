# ESP32-S3 LLM 后台 × Hermes Agent 对接设计

> **历史归档说明（2026-06-14）：** 本文档是 Python `device_gateway` 阶段的服务端设计评审稿。服务端实现已迁移到 `/Users/jiangzhibin/workspace/chatbot-service-java`，本仓库后续只维护固件相关内容。

| 版本 | 日期 | 变更摘要 |
|------|------|---------|
| v1 | 2026-06-14 | 初版：评估把 Nous Research **Hermes Agent** 作为 `device_gateway` 后端的可行性；给出路径 A/B/C 对比、安全收紧清单、部署网络拓扑、超时与会话归属取舍、待拍板决策点 |

> **本文档状态：方案评审稿，未动代码。** 待 §11 决策点拍板后，再按选定路径走实现。
> **更新（2026-06-14）：已选路径 B（Hermes 接管会话/记忆）。详细实现方案见 [`hermes-agent-integration-B-plan.md`](./hermes-agent-integration-B-plan.md)。**
> Hermes 侧技术事实来源为官方文档（见 §12 来源），访问日期 **2026-06-14**；标 `[需实测]` 的项须在搭建时以一手文档/实跑复核（遵循 research-first 第三方一手文档原则）。

---

## 1. 目标与背景

把 `device_gateway` 当前直连的"裸大模型"（DeepSeek `/chat/completions`）替换 / 升级为 **Hermes Agent** —— 一个带持久记忆、技能（skill）学习、工具调用、200+ 模型路由的自治 Agent。期望 ESP32 设备问出的问题，由 Hermes 这个"大脑"来回答，而不仅是一次性补全。

**关键利好（为什么这事很顺）：** `device_gateway/app_core.py` 的 `ChatService.chat()` 本就是标准 OpenAI 调用：

```
POST {base_url}/chat/completions
Authorization: Bearer {api_key}
body: { model, messages, stream:false }   →  读 choices[0].message.content
```

而 Hermes API server 正好暴露 **OpenAI 兼容**的 `/v1/chat/completions`。因此最浅的接法**只换配置、零改代码**。

---

## 2. 现状架构（基于当前代码）

```
ESP32-S3 (esp32s3_wifi_provision.ino)
  │  串口命令 ask <prompt>
  │  POST /api/chat   { device_id, conversation_id?, prompt }
  │  Header: X-Device-Token
  ▼
device_gateway 后台 (server.py · 纯标准库 · 腾讯云 Docker · :8766)
  │  ① require_device()  —— X-Device-Token 鉴权
  │  ② max_prompt_chars 限长（默认 2000）
  │  ③ ConversationStore.history()  —— 取最近 history_limit 轮
  │  ④ build_messages()  —— system_prompt + user_memory + history + prompt
  ▼
ChatService.chat()  →  POST {base_url}/chat/completions   (OpenAI 格式)
  │  base_url=https://api.deepseek.com  model=deepseek-chat  Bearer {api_key}
  │  timeout=60s  stream=False
  ▼
DeepSeek  →  choices[0].message.content
  ▼
⑤ ConversationStore.append_turn()  —— 落 user/assistant 两条
  ▼
返回 { device_id, conversation_id, answer } 给设备
```

**当前承担的职责（这些价值要在对接后保留）：** 设备鉴权（`X-Device-Token`）、prompt 限长、按 `device_id+conversation_id` 的多轮会话、API Key 托管（不下发到固件）、随机路径 + Basic Auth 管理后台。

**配置项**（`data/llm_config.json`，管理页可改）：`base_url` / `model` / `api_key` / `system_prompt` / `user_memory` / `max_prompt_chars` / `history_limit` / `device_token` / 管理凭据。

---

## 3. Hermes API Server 关键事实

| 项 | 值 | 备注 |
|---|---|---|
| 启用方式 | `~/.hermes/.env` 写 `API_SERVER_ENABLED=true` + `API_SERVER_KEY=<强随机>` | |
| 启动 | `hermes gateway` | |
| 默认监听 | `127.0.0.1:8642` | **仅本机**；跨容器 / 跨主机可达性需解决，见 §9 `[需实测]` |
| 鉴权 | `Authorization: Bearer <API_SERVER_KEY>` | 与 device_gateway 现有 `Bearer {api_key}` 完全兼容 |
| 无状态对话端点 | `POST /v1/chat/completions` | OpenAI 兼容，多轮靠请求里带全量 messages |
| 有状态对话端点 | `POST /v1/responses` | 服务端持久化，`previous_response_id` / 命名 `conversation` 串联上下文 |
| 模型选择 | 服务端 `hermes model` 配置 | **请求里的 `model` 字段是装饰性的**，实际模型由 Hermes 服务端定 |
| 流式 | `"stream":true` → SSE，含 `hermes.tool.progress` 事件 | device_gateway 当前 `stream:false`，无需改 |
| 存储上限 | 已存 responses 上限 100，LRU 淘汰 | 影响路径 B 的长会话 |
| 工具暴露 | **完整工具集，含「执行终端命令」** | ⚠️ 见 §8 安全 |
| 多实例 | `hermes profile create <name>` + 独立端口/key | 可做多租户隔离 |

---

## 4. 目标架构（路径 A：把 Hermes 当带工具的大脑）

```
ESP32-S3  ──/api/chat──▶  device_gateway (:8766)  ──/v1/chat/completions──▶  Hermes API server (:8642)
           X-Device-Token   鉴权/限长/会话      Bearer API_SERVER_KEY      工具 + 记忆 + 模型路由
                                                                              │
                                                                              ▼
                                                              OpenRouter / DeepSeek / 本地模型 …
```

设备契约、固件、管理后台**全部不动**；device_gateway 仅改 3 个配置值（§13）。Hermes 落在 device_gateway 之后，对设备完全透明。

---

## 5. 接入路径对比

| 维度 | **A · 零改动网关** | **B · Hermes 接管记忆** | **C · 设备直连 Hermes** |
|---|---|---|---|
| device_gateway 改动 | 改 3 个配置值，**零代码** | 重写 `ChatService` 走 `/v1/responses` + 会话映射；`ConversationStore` 语义改为存 `response_id` | 基本绕过 device_gateway |
| 固件改动 | 无 | 无 | 需改（直连地址 / 鉴权 / 可能 SSE） |
| 会话/记忆归属 | **device_gateway 管**（Hermes 无状态） | **Hermes 管**（吃到记忆 / skill / 学习） | Hermes 管 |
| 吃到 Hermes 核心卖点 | ❌ 仅工具 + 模型路由 | ✅ 记忆 + skill 学习 + 工具 | ✅ |
| 工作量 | 极小（分钟级） | 中（需先出改造方案） | 中大且**不推荐** |
| 主要风险 | 双层 system prompt 叠加；agent 变慢撞 60s 超时 | 100 条 response 上限；会话映射一致性；改动面 | 设备持强 key（terminal 风险）；丢掉现有鉴权/限长/后台 |

**C 为何否决：** ① ESP32 要持有能驱动 terminal 的 Hermes 强 key，泄漏即宿主沦陷；② 丢掉 device_gateway 已有的 `device_token` 鉴权、限长、多设备会话、管理后台；③ 设备侧做 SSE / Bearer 更折腾。**device_gateway 作为"设备网关 + 鉴权 + 限流 + 后台"这层有独立价值，应保留，Hermes 放它后面。**

**推荐：A 起步验证链路 → 按需演进到 B。**

---

## 6. 会话与记忆归属（A 与 B 的本质差异）

这是两条路径的真正分水岭，不是工作量大小，而是**"记住对话"这件事归谁**：

- **路径 A**：device_gateway 的 `ConversationStore` 继续按 `device_id+conversation_id` 存历史，每次把最近 N 轮塞进 `messages` 发给 Hermes；Hermes 当**无状态**补全。结果：Hermes 退化为"带工具的 LLM"，**它的跨会话记忆 / skill 自学习用不上**。
- **路径 B**：把"记历史"交给 Hermes（`/v1/responses` 的 `previous_response_id` 或命名 `conversation`）。device_gateway 只需维护 `device_id+conversation_id → Hermes 会话标识` 的映射，退化为纯网关。结果：吃到 Hermes 的记忆 / skill / 学习，但要处理 100 条 response 上限的 LRU 淘汰、会话映射持久化、改动面。

> 若最终选 B，建议补一条 ADR 记录"会话状态归属从 device_gateway 迁移到 Hermes"这一架构决策（满足"有明显取舍、未来可能被质疑"的 ADR 触发条件）。

---

## 7. 行为差异与超时（务必调整）

1. **超时**：`app_core.py:136` 写死 `timeout=60`。Hermes 是 Agent，可能多步推理 + 跑工具，单轮耗时常**远超**裸 LLM。→ A 路径需把该超时调大（建议先 120~180s 起步实测），并同步评估 ESP32 固件侧 `ask` 的 HTTP 超时。
2. **双层 system prompt**：device_gateway 的 `system_prompt` + `user_memory` 会随 messages 进入 Hermes，而 Hermes 自身也有人格 / 系统提示。两层可能叠加甚至打架。→ A 路径下建议**简化 device_gateway 的 system_prompt**，把人格交给 Hermes（`/personality`）。
3. **限长**：`max_prompt_chars=2000` 对 agent 类输入可能偏紧，按需放开。
4. **流式**：当前 `stream:false`，设备拿到的是一次性完整回答。代价是"Agent 思考期间设备无反馈、等待较久"。短期可接受；若要进度反馈，是后续独立增强（设备侧也要支持）。

---

## 8. 安全设计（重点 · security review）

> ⚠️ **本节涉及鉴权与外部系统集成，属安全敏感，落地代码须人工 review。**

### 8.1 新引入的最大风险：Hermes 暴露终端命令

Hermes API server 文档明确：**API 暴露完整工具集，包含执行终端命令**。把它接在面向 IoT 设备的链路后，等价于"任何能向 ESP32 / `/api/chat` 发 prompt 的人"，理论上能间接驱动 Hermes 在其宿主机上执行命令。**这是本次对接的头号风险。**

强制措施（上线前逐条落实）：

1. **收紧 Hermes 工具集** —— 禁用 terminal / 文件写 / 危险工具，只保留必要的（如检索、问答类）。`[需实测]` 具体工具开关项以 Hermes `hermes tools` / 配置为准。
2. **强 `API_SERVER_KEY`** —— 高熵随机，且**只在内网 / Docker 网络可达，绝不映射公网端口**（与 device_gateway 的 `8766` 不同，Hermes 的 `8642` 不应进腾讯云安全组放行列表）。
3. **保留 `device_token` 作第一道门** —— 设备侧鉴权不能因为后端换了 Hermes 而放松。
4. **最小权限部署** —— Hermes 容器 / 进程以非 root、受限文件系统、独立网络运行；即便工具被诱发，爆炸半径受限。

### 8.2 叠加在现有风险之上

`docs/deployment.md §9` 已记录 device_gateway 当前 **HTTP 明文**（Basic Auth base64、公网 8766）。Hermes 接入不改变这一点，但**放大了后果**：明文链路 + 后端能跑命令 = 风险等级上升。→ 建议把"上 HTTPS"从 deployment.md 的可选 TODO 提升为对接 Hermes 的前置项。

### 8.3 凭据语义变化（避免混淆）

切到 Hermes 后，device_gateway 配置里的 `api_key` 含义从"DeepSeek key（直接计费调模型）"变成"**Hermes 网关鉴权 key（`API_SERVER_KEY`）**"；真正调底层模型的 provider key（OpenRouter / DeepSeek / Nous Portal）改在 **Hermes 侧**配置。计费口径随之转移到 Hermes。

---

## 9. 部署与网络拓扑

现状：device_gateway 在腾讯云 CVM（`203.195.202.54`，4 核 / 3.6G，Ubuntu 24.04）Docker Compose 部署，端口 8766。Hermes 部署有两种位置：

| 方案 | 拓扑 | 优点 | 风险 / `[需实测]` |
|---|---|---|---|
| **同机同 Docker 网络**（推荐） | 同一 CVM 内新增 hermes 容器，与 device_gateway 同 compose 网络；device_gateway 用 `http://hermes:8642/v1` 互访 | 不暴露 8642 到公网；运维集中 | **3.6G 内存对 Hermes 是否够**（uv/node/ffmpeg + 若跑本地模型更紧）`[需实测]`；Hermes 默认仅听 127.0.0.1，需让它监听容器网络可达地址 `[需实测]` |
| **独立主机** | Hermes 单独一台，device_gateway 经内网 / 加密隧道访问 | 资源隔离；互不影响 | 多一台机器成本；内网打通 |

**Hermes 监听地址问题** `[需实测]`：官方文档给的默认是 `127.0.0.1:8642`。要让另一个容器访问，需确认 Hermes 是否提供 `API_SERVER_HOST` 类绑定配置；若无，兜底用同机反向代理（nginx/caddy）或共享网络命名空间。**此项是搭建时第一个要验证的点。**

> 资源建议：让 Hermes 只做"模型路由"（调远程 OpenRouter/DeepSeek，不在本机跑大模型），把 3.6G 压力降到最低。

---

## 10. 验证场景（Check —— 选定路径实现后必须通过）

### 场景 A1 —— 链路连通（路径 A）
| 步骤 | 操作 | 预期 |
|---|---|---|
| A1-1 | Hermes 起服务，curl `/v1/chat/completions` 带 Bearer | 200 + 正常回答 |
| A1-2 | device_gateway 后台改 3 配置值后，用管理页"测试聊天" | 返回 answer，且来自 Hermes |
| A1-3 | `device_simulator.py` 连续两轮提问 | 第二轮能延续上下文（device_gateway history 生效） |
| A1-4 | 故意问需较长思考的问题 | 不因 60s 超时报错（验证超时已调大） |

### 场景 A2 —— 鉴权与隔离
| 步骤 | 操作 | 预期 |
|---|---|---|
| A2-1 | 不带 `X-Device-Token` 打 `/api/chat`（已配 token 时） | 401/403，未触达 Hermes |
| A2-2 | 从公网直接 curl Hermes `:8642` | **连不通**（未对公网放行） |

### 场景 B（若选 B，另补）
会话映射建立 / 跨"会话"记忆延续 / response LRU 淘汰后的降级行为。

---

## 11. 待拍板决策点

1. **路径**：A（先零改动验证）/ B（直奔 Hermes 管记忆）/ 先 A 后 B（推荐）。
2. **Hermes 部署位置**：同机同 Docker 网络（推荐）/ 独立主机。
3. **底层模型**：Hermes 后面接哪个 provider（OpenRouter 一篮子 / 继续 DeepSeek / Nous Portal）。
4. **HTTPS 是否设为前置**：建议是（§8.2）。
5. **是否做工具收紧的最小白名单**：建议是且必须（§8.1）。

---

## 12. 来源（一手文档 · 访问日期 2026-06-14）

- 仓库：https://github.com/NousResearch/hermes-agent
- API Server：https://hermes-agent.nousresearch.com/docs/user-guide/features/api-server
- 编程集成：https://hermes-agent.nousresearch.com/docs/developer-guide/programmatic-integration
- 集成总览：https://hermes-agent.nousresearch.com/docs/integrations/

> 标 `[需实测]` 项（监听地址绑定、工具开关、资源占用、`/v1/responses` 会话参数细节）须在搭建时以官方文档最新版 + 实跑复核，发现与本文不符以官方为准并回填本文档。

---

## 13. 附录 · 路径 A 精确配置

device_gateway 管理页（`/admin/<secret>`）改三项，落到 `data/llm_config.json`：

| 后台字段 | 配置键 | 改成 | 说明 |
|---|---|---|---|
| 大模型 URL | `base_url` | `http://<hermes可达地址>:8642/v1` | `ChatService` 拼 `{base_url}/chat/completions`，rstrip 尾斜杠后正好得 `…/v1/chat/completions` ✓ |
| API Key | `api_key` | Hermes 的 `API_SERVER_KEY` | 作为 `Bearer` 发出 |
| 模型名 | `model` | `hermes-agent`（或 profile 名） | Hermes 端 model 字段为装饰性，填啥都行 |

Hermes 侧 `~/.hermes/.env`：

```bash
API_SERVER_ENABLED=true
API_SERVER_KEY=<高熵随机串>
# 监听地址绑定方式 [需实测]；同机同网络兜底见 §9
```

冒烟 curl（搭好后先单测 Hermes，再切 device_gateway）：

```bash
curl http://<hermes可达地址>:8642/v1/chat/completions \
  -H "Authorization: Bearer <API_SERVER_KEY>" \
  -H "Content-Type: application/json" \
  -d '{"model":"hermes-agent","messages":[{"role":"user","content":"你好，一句话自我介绍"}],"stream":false}'
```

> 切换前先 `cat /opt/device_gateway/data/llm_config.json` 备份原 `base_url`/`model`/`api_key`，便于回滚到 DeepSeek。

---

## 14. 语音交互链路全景（roadmap · 唤醒词 / 流式 / ASR / TTS 在哪做）

> 本项目目标是**单用户语音机器人**，硬件经语音与机主交互。本节定位语音链路各层的位置，供后续分阶段实施。`device_gateway` 现有 `/api/voice/chat`（返回 `voice_not_ready`）即这条链路的入口占位。

### 14.1 分层与位置

```
[ESP32-S3 设备本地]                                  [云端 / device_gateway + Hermes]
 I2S 麦克风
   │ 常驻低功耗监听
   ▼
 ① 唤醒词 KWS（必须本地）──检测到──▶ ② 录音 + VAD（本地）
                                       │ 音频流（WebSocket / HTTP 流式）
                                       ▼
                                  ③ ASR 语音转文字（云端）
                                       ▼
                             ④ device_gateway → Hermes（LLM，本设计主体）
                                       │ 文本流式（SSE）
                                       ▼
                                  ⑤ TTS 文字转语音（云端，流式）
   ⑥ 喇叭播放（本地）◀────音频流回传──────┘
```

| 层 | 放哪 | 为什么 | 选型 |
|---|---|---|---|
| ① 唤醒词 KWS | **设备本地（强制）** | 要常驻听麦克风，不可能把音频一直流后端（功耗/流量/延迟/隐私）| **ESP-SR WakeNet9**（乐鑫官方，支持 S3，建议 8MB PSRAM）；或 TFLite Micro 自训 |
| ② 录音 / VAD | 设备本地 | 唤醒后才录音、静音停录 | ESP-SR AFE（降噪 / AEC / VAD）|
| ③ ASR | **云端** | 自由语音识别 ESP32 跑不动 | 云 ASR（Whisper / 腾讯云 / 讯飞等）`[待选型]` |
| ④ LLM | 云端（device_gateway→Hermes）| 本设计主体 | Hermes（路径 B）|
| ⑤ TTS | **云端** | 高质量合成 ESP32 跑不动 | 云 TTS，流式输出 `[待选型]` |
| ⑥ 播放 | 设备本地 | 喇叭 | I2S 功放 MAX98357A + 喇叭 |

### 14.2 唤醒词的三个现实坑

1. **框架**：ESP-SR 基本只在 **ESP-IDF** 下使用（官方示例均 `idf.py`），而当前固件 `esp32s3_wifi_provision` 是 **Arduino 框架**。要用 ESP-SR 需把工程迁到 ESP-IDF（或 Arduino-as-component），或改用 TFLite Micro 自训轻量 KWS。`[需决策]`
2. **自定义唤醒词成本高**：自定义词需 500+ 人录音样本送乐鑫训练 → 现实做法先用**内置唤醒词**（如 "Hi ESP"），定制留后期。
3. **音频硬件未接**：需 I2S MEMS 麦克风（INMP441）+ I2S 功放（MAX98357A）+ 喇叭；README 现状为"硬件音频模块接入前"。

### 14.3 流式（为什么必须 + 影响哪里）

整条语音链路体验取决于**首字延迟**：等 Hermes 跑完多步 + 工具再整段返回会很慢，语音场景尤其难忍。因此 ④→⑤ 必须流式：

- **Hermes 侧**：`/v1/chat/completions` 传 `stream:true` → SSE，逐段 delta + `hermes.tool.progress` 工具进度事件（可驱动设备"思考中"提示）。
- **device_gateway 侧**：当前 `stream:false` 写死、`/api/chat` 一次性 JSON。需新增 **SSE 透传**（如 `/api/chat/stream`）：向 Hermes 发 `stream:true`，边收边转发给设备 / TTS。
- **设备侧**：读 HTTP 流（Arduino `http.getStream()` 逐块解析 SSE）。
- **取文本变化**：非流式取 `choices[0].message.content`；流式改为累积 SSE delta（B-plan 路径 B-1 的"响应解析不改"仅适用非流式阶段）。

### 14.4 建议实施阶段（每段独立可验证）

1. **阶段一（B-1 非流式）**：先把"文本进 / 文本出 + Hermes 记忆"链路跑通（本设计 + B-plan）。用 `device_simulator` / 串口 `ask` 验证。
2. **阶段二（流式）**：device_gateway 加 SSE 透传，设备 / 模拟器支持读流，验证首字延迟改善。
3. **阶段三（语音）**：接音频硬件 + 唤醒词（ESP-SR，可能切 IDF）+ 云 ASR / TTS，打通 `/api/voice/chat`。

### 14.5 来源（一手 · 2026-06-14）

- ESP-SR / WakeNet：https://github.com/espressif/esp-sr 、https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/README.html
- 自定义唤醒词流程：https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/ESP_Wake_Words_Customization.html
- ESP32-S3 语音助手参考架构：Hackster / Hackaday / Home Assistant ESP32-S3-BOX 等社区实践
</content>
</invoke>
