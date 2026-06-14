# 路径 B 实现方案 —— Hermes Agent 接管会话/记忆

| 版本 | 日期 | 变更摘要 |
|------|------|---------|
| v1 | 2026-06-14 | 初版：路径 B（Hermes 接管会话/记忆）详细实现方案。确定子路径 B-1（`/v1/chat/completions` + session headers），给出双层 ID 映射、数据模型变化、改动清单、spike 验证、AC+测试骨架、ADR 草案、回滚 |

> **状态：实现方案评审稿，未动代码。** 总体可行性评估见 [`hermes-agent-integration-design.md`](./hermes-agent-integration-design.md)；本文聚焦 B 的落地。
> Hermes 侧事实来源为官方文档 + 源码 `gateway/platforms/api_server.py`（访问日期 **2026-06-14**，见 §14）。标 `[需 spike]` 项必须在写实现代码前先实跑验证；标 `[待拍板]` 项需你决策。

---

## 1. 目标与范围

把"记住对话 + 长期记忆 + skill 学习"从 llm_admin 迁移到 Hermes，发挥 Hermes 核心价值。

**对设备的契约完全不变**（`/api/chat` 的入参 `device_id`/`conversation_id`/`prompt`、出参 `answer` 全不动）→ **固件无需改动、无需重新烧录**。改动全部收敛在 llm_admin → Hermes 这一段。

---

## 2. 关键决策：B 的三条子路径

| 子路径 | 机制 | endpoint | 响应解析 | 100-LRU 限制 | 改动量 | 取舍 |
|---|---|---|---|---|---|---|
| **B-1（选定）** | `/v1/chat/completions` + `X-Hermes-Session-Id` + `X-Hermes-Session-Key` 头 | **不换** | **不改**（`choices[0].message.content`）| **不触碰**（走 state.db，非 response store）| **最小** | 依赖 chat/completions 支持 session 头加载历史 `[需 spike S-1]` |
| B-2 | `/v1/responses` + `conversation_id` + `store:true` | 换 `/responses` | 改（解析 `output[].content[].output_text`）| **受限**（全局 100 条 LRU 淘汰旧会话快照）| 中 | OpenAI Responses 标准，但多设备长尾会话会被淘汰 |
| B-3 | `/api/sessions/{id}/chat` | 换 Hermes 私有端点 | 改（`message.content`）| 不受限 | 最大 | 需自管 session 生命周期；非 OpenAI 契约、绑定 Hermes |

**选 B-1 的理由：** 改动最小（不换 endpoint、不改解析）、不碰 100 条 LRU 限制、双层会话头与现有 `device_id`/`conversation_id` 一一对应。**前提**是 spike S-1 验证通过；若不通过，回退 B-3（B-2 因 LRU 限制不适合多设备）。

---

## 3. 核心设计：双层 ID 映射

Hermes 把"会话"拆成两层，正好对上 llm_admin 已有的两层 ID：

| llm_admin | → Hermes header | Hermes 语义 | 效果 |
|---|---|---|---|
| `conversation_id`（一次连续对话）| `X-Hermes-Session-Id` | 短期 transcript，`/new` 时轮换 | 同一对话延续上下文；新会话 = 新 transcript |
| `device_id`（识别设备）→ 取值见 §6 | `X-Hermes-Session-Key` | 长期记忆 scope，跨 transcript 持久（≤256 字符、无控制符）| **单用户：用固定 `owner` key，全设备共享"我"的长期记忆（§6）** |

> 语义升级：原来"换会话 = 历史清零"；B 之后"换会话（新 Session-Id）但同设备（同 Session-Key）→ 短期上下文重置、长期记忆延续"。这正是 Hermes 的记忆价值所在。

---

## 4. 调用契约变化（llm_admin → Hermes）

| | 现状（直连 DeepSeek / 路径 A） | B-1 之后 |
|---|---|---|
| URL | `{base_url}/chat/completions` | **不变** |
| 鉴权头 | `Authorization: Bearer {api_key}` | **不变**（api_key = Hermes `API_SERVER_KEY`）|
| 新增头 | — | `X-Hermes-Session-Id: {conversation_id}`、`X-Hermes-Session-Key: {device_id}` |
| messages | system + user_memory + **本地 history** + prompt | system（[待拍板]见下）+ prompt，**不再回灌本地 history**（Hermes 从 state.db 取）|
| 响应取值 | `choices[0].message.content` | **不变** |
| timeout | `60`（写死） | 配置化，默认调大（建议 120~180s 起，实测）|

**system_prompt / user_memory 的去向** `[待拍板]`：B 下记忆交给 Hermes，建议把人格/记忆迁到 Hermes 端（`/personality` + memory provider），llm_admin 停止注入 `user_memory`（避免与 Hermes 记忆重复打架）；`system_prompt` 可保留极简一句或一并交给 Hermes。

---

## 5. 数据模型变化（ConversationStore 的命运）

`ConversationStore` 原本两个职责：① 存历史 ② 回灌历史给模型。B-1 下 ② 移交 Hermes（state.db）。① 怎么处理：

| 方案 | 本地 conversations.json | 回灌历史 | 评价 |
|---|---|---|---|
| (a) 纯透传 | 不再存 messages | 否 | 最干净，但丢失本地可观测性 + 回滚困难 |
| **(b) 审计镜像（推荐）** | **仍记 user/assistant**（仅作审计/备份/可回滚）| 否 | 保留可观测性、不破坏现有数据结构、回滚到 A/DeepSeek 时可重新启用回灌 |

推荐 **(b)**：`append_turn()` 保留（落盘审计），`history()` 不再被 `/api/chat` 用于回灌。`new_conversation()` 仍生成 `conversation_id`（用作新的 `X-Hermes-Session-Id`），语义变为"开启新 Hermes transcript"。`history_limit` 配置项在 B-1 下失效（保留兼容或标记废弃）。

---

## 6. 记忆模型（单用户场景）

> **本项目为单用户**：机器人只为机主一人所用。因此 Hermes 多用户 / 多实例记忆隔离的已知局限（[#6320](https://github.com/NousResearch/hermes-agent/issues/6320)、[#11430](https://github.com/NousResearch/hermes-agent/issues/11430)）**不适用**，隔离顾虑消失，设计大幅简化。
> 本节为单用户结论，**覆盖下文 §3 表 Session-Key 行、AC-3 / AC-7、R-1、S-3 中关于"多设备隔离"的表述**（单用户下均简化或不适用）。

### 6.1 Session-Key 策略：全设备共享"我"的记忆

单用户下，期望的恰恰是**跨设备记忆延续**（客厅设备说的，卧室设备也记得）。因此：

- `X-Hermes-Session-Key` 用**固定机主 key**（如 `owner`），所有硬件共享同一长期记忆 scope —— **不再按 `device_id` 分**。
- `X-Hermes-Session-Id` 仍用 `conversation_id` 区分不同对话轮次（短期 transcript）。
- **单 Hermes profile 足够**，无需 per-device profile / 多实例。

> `device_id` 不再用于记忆隔离，但仍保留用于 llm_admin 的设备识别 / 审计镜像 / `device_token` 鉴权。

### 6.2 长期记忆依赖 memory provider

Hermes 跨会话"记住我"依赖 **memory provider（如 Honcho）**。不配 provider 时只剩 `Session-Id` 短期 transcript，`Session-Key` 长期记忆退化。→ 要 B 的长期记忆卖点，需决定是否接入 Honcho `[待拍板]` + `[需 spike S-2]`。

### 6.3 安全焦点转移

单用户下安全焦点从"记忆隔离"转为**物理访问控制**（防别人对着我的设备说话）——`device_token` 仍是第一道门；记忆层面无隔离顾虑。

---

## 7. 代码改动清单（B-1）

| # | 文件 | 改动 | 级别 |
|---|---|---|---|
| 1 | `llm_admin/app_core.py` | `ChatService.chat()` 增加 `session_id` / `session_key` 入参；注入两个 `X-Hermes-*` 头；`build_messages()` 不再拼 history；`timeout` 改为可配置（新增 `request_timeout` 配置项，默认 >60s）；新增开关 `hermes_session_mode`（on 时启用 B-1 行为，off 退回 A/DeepSeek 行为，便于回滚）| 核心 |
| 2 | `llm_admin/server.py` | `/api/chat` handler：把 `device_id`→session_key、`conversation_id`→session_id 传入 `chat()`；按开关决定是否回灌 history（off 保持原样）| 核心 |
| 3 | `llm_admin/conversation.py` | 保留 `append_turn`（审计镜像）；`history()` 保留但 B-1 下不参与回灌（加注释说明双模式）| 小 |
| 4 | `data/llm_config.json` 配置项 | 新增 `request_timeout`、`hermes_session_mode`（及可能的 `system_prompt` 策略）；管理页表单同步 | 小 |
| 5 | `tests/test_llm_admin_core.py`、`tests/test_llm_admin_http.py` | 按 §9 AC 增量测试（mock transport 断言头/不回灌/取值/开关）| 测试 |
| 6 | `docs/deployment.md` | §8 接口契约摘要补充 llm_admin→Hermes 调用变化；部署补 Hermes 容器 + `API_SERVER_HOST=0.0.0.0` + state.db 持久化卷 | 文档 |
| 7 | `docs/adr/ADR-001-session-state-ownership.md`（新建）| 记录"会话/记忆状态归属迁移"决策（§11）| 文档 |

> 改动遵循增量验证：先做 #1+#2 的开关骨架（off 时行为与现状完全一致）→ spike 验证 → 打开开关跑通 → 再清理。

---

## 8. Spike 验证清单（写实现代码前必须先实跑）

| # | 验证 | 不通过的后果 |
|---|---|---|
| **S-1** | `/v1/chat/completions` + `X-Hermes-Session-Id` 是否真的从 state.db 加载历史（不需本地回灌）；同一 Session-Id 第二轮能否延续上下文 | B-1 不成立，回退 B-3 |
| **S-2** | `X-Hermes-Session-Key` 长期记忆是否需要先配 Honcho 等 memory provider 才生效 | 决定是否引入 Honcho 依赖 |
| **S-3** | 两个不同 Session-Key 的记忆是否实际隔离（不串）；同 Session-Key 跨 Session-Id 是否延续 | 决定隔离策略（§6.1）|
| **S-4** | 在你实际部署的 Hermes 版本上，`choices[0].message.content` 是否稳定返回最终文本（agent 多步/工具场景下）| 可能需改响应解析 |
| **S-5** | Hermes 容器 `API_SERVER_HOST=0.0.0.0` 后，llm_admin 容器经 docker 网络能否访问 `:8642` | 部署网络方案调整 |

> Spike 用 `curl` + 一个临时 Hermes 实例完成，不碰 llm_admin 代码。S-1/S-3 通过是 B-1 推进的前置门。

---

## 9. 验收条件（AC）+ 测试骨架

按 test-skeleton-lock：每条 AC 先写一个 FAIL 测试骨架，编码让其转 PASS，编码阶段不改骨架断言。

| AC | 描述 | 测试要点（mock transport）|
|---|---|---|
| AC-1 | `hermes_session_mode=on` 时，`/api/chat` 调 Hermes 带 `X-Hermes-Session-Id={conversation_id}` 且 `X-Hermes-Session-Key={device_id}` | 捕获 transport 收到的 headers 断言 |
| AC-2 | on 时 `messages` 不含本地历史（只有 system?+当前 prompt）| 断言 payload.messages 长度/内容 |
| AC-3 | 同 device_id、不同 conversation_id → 两次请求 Session-Id 不同、Session-Key 相同 | 两次调用断言头 |
| AC-4 | 返回给设备的 `answer` 仍取自 `choices[0].message.content`，出参结构不变 | 断言响应体 |
| AC-5 | `request_timeout` 可配置且默认 >60s，透传到 transport | 断言 transport 收到的 timeout |
| AC-6 | 审计镜像：on 时 `append_turn` 仍写入 conversations.json | 断言落盘 |
| AC-7 | device_id 作 Session-Key 越界（>256 或含控制符）→ 安全降级（截断/拒绝，明确行为）| 边界用例 |
| AC-8 | `hermes_session_mode=off` 时行为与现状完全一致（回灌 history、无 X-Hermes 头）| 回归保护 |

---

## 10. 风险

| # | 风险 | 处置 |
|---|---|---|
| R-1 | 多用户记忆隔离 Hermes 仍在完善（#6320/#11430）| 软隔离起步 + spike S-3 实测；强隐私场景再评估 profile 方案 |
| R-2 | 长期记忆依赖 Honcho，未配则退化 | spike S-2 确认；[待拍板]是否引入 |
| R-3 | B-1 依赖 chat/completions+session 头加载历史 | spike S-1 为前置门，不过则 B-3 |
| R-4 | state.db 在 Hermes 容器，记忆全在那 | 纳入持久化卷 + 备份（类比 llm_admin 的 data 卷）|
| R-5 | 回滚需要本地历史仍在 | 选数据模型方案 (b) 审计镜像 + `hermes_session_mode` 开关 |
| R-6 | agent 变慢、双层 system prompt | timeout 调大 + 精简本地 system_prompt（§4）|

---

## 11. ADR 草案 —— 会话/记忆状态归属迁移

- **决策**：会话短期上下文 + 长期记忆的存储与拼接，从 llm_admin（`ConversationStore` 回灌）迁移到 Hermes（state.db + memory provider）。llm_admin 降级为网关 + 审计镜像。
- **取舍**：得到 Hermes 记忆/skill/学习；代价是记忆落在 Hermes（隔离强度受限、需 memory provider、state.db 成为关键数据）。
- **替代方案**：保持 llm_admin 自管历史（路径 A）——简单但吃不到 Hermes 记忆。
- **可逆性**：`hermes_session_mode` 开关 + 本地审计镜像保证可回滚。
- 落地时补全为正式 `docs/adr/ADR-001-session-state-ownership.md`。

---

## 12. 回滚方案

1. `hermes_session_mode=off` → llm_admin 立即回到本地回灌 history 行为；
2. `base_url`/`api_key`/`model` 改回 DeepSeek → 完全回到当前线上状态；
3. 本地审计镜像（方案 b）保证回滚后历史不丢。
> 切换前 `cat /opt/llm_admin/data/llm_config.json` 备份。

---

## 13. 待拍板决策点

1. **确认子路径 B-1**（先 spike S-1，通过则走，否则 B-3）。
2. **记忆隔离**：单 profile + Session-Key 软隔离（推荐起步）/ 其他。
3. **是否引入 Honcho memory provider**（要长期记忆卖点大概率需要）。
4. **system_prompt / user_memory 去向**：迁到 Hermes（推荐）/ llm_admin 保留极简。
5. **ConversationStore**：审计镜像方案 (b)（推荐）/ 纯透传 (a)。
6. **Hermes 部署位置 + 底层模型**（承接总设计 §11：同机同 Docker 网络 + OpenRouter/DeepSeek/Nous Portal）。
7. **流式落地节奏**：建议分步——先 B-1 非流式验证记忆，再上 SSE 流式（详见 [`hermes-agent-integration-design.md`](./hermes-agent-integration-design.md) §14 语音链路）。

---

## 14. 来源（一手 · 访问日期 2026-06-14）

- 源码 API server：https://github.com/NousResearch/hermes-agent/blob/main/gateway/platforms/api_server.py
- API Server 文档：https://hermes-agent.nousresearch.com/docs/user-guide/features/api-server
- 编程集成：https://hermes-agent.nousresearch.com/docs/developer-guide/programmatic-integration
- Memory Providers：https://hermes-agent.nousresearch.com/docs/user-guide/features/memory-providers
- 记忆隔离局限：[issue #6320](https://github.com/NousResearch/hermes-agent/issues/6320)、[issue #11430](https://github.com/NousResearch/hermes-agent/issues/11430)、[issue #9514](https://github.com/NousResearch/hermes-agent/issues/9514)

> 标 `[需 spike]` 项以实际部署的 Hermes 版本实跑为准，发现与本文不符以实测/官方为准并回填。
