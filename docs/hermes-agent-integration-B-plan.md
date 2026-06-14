# 路径 B 实现方案 —— Hermes Agent 接管会话/记忆

| 版本 | 日期 | 变更摘要 |
|------|------|---------|
| v1 | 2026-06-14 | 初版：路径 B（Hermes 接管会话/记忆）详细实现方案。确定子路径 B-1（`/v1/chat/completions` + session headers），给出双层 ID 映射、数据模型变化、改动清单、spike 验证、AC+测试骨架、ADR 草案、回滚 |
| v2 | 2026-06-14 | 据 Hermes 官方文档原文核实：**B-1 核心假设被证伪**（`/v1/chat/completions` 是无状态端点，`X-Hermes-Session-Id` 不会服务端加载历史），**改选 B-2（`/v1/responses` 服务端有状态）**。据真实 responses 契约重写 §3 映射 / §4 调用契约 / §5 数据模型 / §7 改动清单 / §8 spike / §9 AC / §13 决策点。B-2 内部「`previous_response_id` 链（B-2a）vs `conversation` 命名会话（B-2b）」两变体待 **S-A** spike 拍板，本文不写死 |
| v3 | 2026-06-14 | **已实现落地**（device_gateway 单测 19 项通过）：最终选 **B-2b 命名会话**（`conversation_id` 透传，零本地会话状态）；按用户决策**彻底移除 off/DeepSeek 回退**、删除 `ConversationStore`、精简配置、中间件**改名 `llm_admin`→`device_gateway`**。§4–§9 保留 v2 评审作演进记录，最终状态以 §0 + 代码 + ADR-001 为准 |

> **状态：实现方案评审稿，未动代码。** 总体可行性评估见 [`hermes-agent-integration-design.md`](./hermes-agent-integration-design.md)；本文聚焦 B 的落地。
> Hermes 侧事实来源为官方文档 + 源码（GitHub raw 原文，访问日期 **2026-06-14**，见 §14）。标 `[需 spike]` 项必须在写实现代码前先实跑验证；标 `[待拍板]` 项需你决策。
> **v2 关键纠偏：** v1 选定的 B-1 依赖「chat/completions + session 头让 Hermes 服务端记历史」，此假设已被官方文档原文直接否定（详见 §2）。目标（Hermes 管会话/记忆）不变，改用官方真正支持服务端有状态的 `/v1/responses` 端点实现。

> **§0 最终落地（v3 / 权威）—— 与 v2 评审稿的差异：**
>
> | 维度 | v2 评审稿 | v3 实际落地 |
> |---|---|---|
> | 串联机制 | B-2a/B-2b 待 S-A 拍板 | **B-2b 命名会话**（`conversation=conversation_id`）；S-A 转部署后验证（`hermes-spike.sh`），不过则回退 B-2a |
> | DeepSeek 回退 | 保留 `hermes_session_mode` + off | **彻底删除**，只剩 Hermes；换模型在 Hermes 端换 provider |
> | ConversationStore | 保留作审计镜像 | **整模块删除**；`conversation_id` 由网关 uuid 生成、纯透传 |
> | 配置 | 含 system_prompt/user_memory/history_limit/hermes_session_mode | **精简掉这 4 项** |
> | instructions | system_prompt→instructions | **不注入**，人格全归 Hermes |
> | 中间件名 | llm_admin | **device_gateway** |
> | AC | AC-1~9 | off/AC-8 删；AC-9 仅 B-2a 需要；余对齐纯网关 |
>
> 决策详见 [ADR-001](./adr/ADR-001-session-state-ownership.md)。

---

## 1. 目标与范围

把"记住对话 + 长期记忆 + skill 学习"从 device_gateway 迁移到 Hermes，发挥 Hermes 核心价值。

**对设备的契约完全不变**（`/api/chat` 的入参 `device_id`/`conversation_id`/`prompt`、出参 `answer` 全不动）→ **固件无需改动、无需重新烧录**。改动全部收敛在 device_gateway → Hermes 这一段。

---

## 2. 关键决策：选定 B-2（`/v1/responses`），B-1 已证伪

### 2.1 B-1 为何出局（一手文档原文）

v1 选定的 B-1 立论是：`/v1/chat/completions` + `X-Hermes-Session-Id` 头能让 **Hermes 服务端从 state.db 加载历史、客户端不再回灌 messages**。官方 `api-server.md` 原文直接否定：

> `/v1/chat/completions`：**"Stateless — the full conversation is included in each request via the `messages` array."**
> `X-Hermes-Session-Id` does not load prior history server-side; clients must send the complete message array each turn.

即该端点上「服务端记短期历史、客户端不带 messages」做不到。B-1 会退化成两种结局，无第三种：① 照设计「不回灌 history」→ 短期多轮断裂（每轮无上文）；② 仍回灌 → 会话职责没迁走，等于路径 A。**这与本文档自身引用的 design §3「chat/completions 无状态、多轮靠请求带全量 messages」原本就一致——v1 在选 B-1 时未接住该事实。**`state.db` 存的是 Hermes agent 自身状态/记忆，不等于「替客户端回灌对话历史给 chat/completions」。

### 2.2 子路径全景（达成「Hermes 接管会话/记忆」的实现选项）

| 子路径 | 端点 / 机制 | 短期上下文谁管 | 长期记忆 | 改动量 | 取舍 / 结论 |
|---|---|---|---|---|---|
| ~~B-1~~ | chat/completions + `X-Hermes-Session-Id` | 想交 Hermes | session-key | — | ❌ 端点无状态，做不到，**已否决** |
| **B-2a（选定·变体一）** | `/v1/responses` + `previous_response_id` 链 | **Hermes** | session-key | 中 | OpenAI 标准；需维护「conversation_id→最新 response_id」每轮回写；碰 100-LRU 失效要降级处理 |
| **B-2b（选定·变体二，倾向）** | `/v1/responses` + `conversation:"<名字>"` 命名会话 | **Hermes** | session-key | **小** | `conversation=conversation_id` 直接透传，ConversationStore 几乎不改；命名会话语义/上限未知，**待 S-A** |
| 乙（折中·未采纳，留档） | 路径 A 本地回灌 **+ `X-Hermes-Session-Key`** | device_gateway 本地 | **Hermes** | 最小 | 不接管短期、只接长期记忆/skill/工具；拿 80% 价值、风险最低。作为 B-2 受阻时的退路 |
| B-3（兜底·未采纳） | `/api/sessions/{id}/chat` 私有端点 | Hermes | session-key | 大 | 绑定 Hermes 私有契约、非 OpenAI 标准；端点契约未单独核实。仅 B-2 不可行时启用 |
| ~~/v1/runs~~ | runs API | — | — | — | 偏异步任务执行，非同步多轮对话设计，**排除** |

### 2.3 选 B-2 的理由 + 两变体待定

- `/v1/responses` 是官方**唯一**的服务端有状态途径（原文见 §4），正是「Hermes 管会话」的正道，也是 design 文档原始的路径 B 设想。
- 单用户场景（§6）下 **100-LRU 不构成阻塞**：顺序对话只需持有最新 `response_id` 往前串（B-2a），或用命名会话规避（B-2b）；v1 用「多设备长尾会话被淘汰」否决 responses，与 §6 单用户简化自相矛盾，该否决理由不成立。
- **B-2a vs B-2b 取决于 S-A spike**（命名会话是否持久、是否受上限、与 session-id/key 关系）。**B-2b 改动更小，为倾向项，但未验证前不写死。** 见 §5 / §8。

---

## 3. 核心设计：双层 ID 映射

Hermes 把"会话"拆成两层，正好对上 device_gateway 已有的两层 ID（B-2 下承载方式与 B-1 不同，但映射关系不变）：

| device_gateway | → Hermes 承载方式 | Hermes 语义 | 效果 |
|---|---|---|---|
| `conversation_id`（一次连续对话）| `/v1/responses` 的 `conversation` 字段（B-2b）**或** `previous_response_id` 链（B-2a）`[待 S-A]` | 服务端会话状态 / 短期 transcript | 同一对话延续上下文；新会话 = 新 `conversation` 名 / 不带 `previous_response_id` |
| `device_id`（识别设备）→ 取值见 §6 | `X-Hermes-Session-Key` | 长期记忆 scope，跨 transcript 持久（≤256 字符、无控制符）| **单用户：用固定 `owner` key，全设备共享"我"的长期记忆（§6）** |

> `X-Hermes-Session-Id` 头在 responses 下是否仍需传（用于 transcript 标识），文档未明说耦合，列入 `[需 spike S-A]`；但**它不再是上下文加载的关键**——短期上下文由 responses 的 `conversation`/`previous_response_id` 承载。
> 语义升级：原来"换会话 = 历史清零"；B-2 之后"换会话（新 conversation / 不带 previous_id）但同设备（同 Session-Key）→ 短期上下文重置、长期记忆延续"。这正是 Hermes 的记忆价值所在。

---

## 4. 调用契约变化（device_gateway → Hermes）

| | 现状（直连 DeepSeek / 路径 A） | B-2（`/v1/responses`） |
|---|---|---|
| URL | `{base_url}/chat/completions` | **`{base_url}/responses`**（`base_url` 末尾配到 `/v1`；endpoint 路径在代码中写死，**需改**，非仅改配置）|
| 鉴权头 | `Authorization: Bearer {api_key}` | **不变**（api_key = Hermes `API_SERVER_KEY`）|
| 新增头 | — | `X-Hermes-Session-Key: owner`（长期记忆 scope）；`X-Hermes-Session-Id` 是否传 `[需 spike S-A]` |
| 请求体 | `{model, messages:[system+user_memory+history+user], stream:false}` | `{model, input:"<prompt>", instructions:"<精简 system>", store:true, previous_response_id|conversation, stream:false}`，**不再带 history / user_memory** |
| 多轮串联 | 本地 history 回灌进 messages | `previous_response_id` 链（B-2a）**或** `conversation` 命名会话（B-2b）`[待 S-A]` |
| 响应取值 | `choices[0].message.content` | **遍历 `output[]`**：取 `type=="message" && role=="assistant"` 项的 `content[]` 中 `type=="output_text"` 的 `text`；**同时记录响应顶层 `id` 作下轮 `previous_response_id`**（B-2a）。无顶层便捷 `output_text` 字段 |
| timeout | `60`（写死） | 配置化，默认调大（agent 多步推理 + 工具，比裸 LLM 更慢；建议 120~180s 起，实测）|

**请求/响应骨架（一手契约）：**
```jsonc
// 请求
POST {base_url}/responses
{ "model":"hermes-agent", "input":"用户这一句话",
  "instructions":"精简的人格/系统提示", "store":true,
  "previous_response_id":"resp_abc123" }   // 或 "conversation":"<conversation_id>"；首轮都不带
// 响应
{ "id":"resp_xxx",                          // ← 下一轮的 previous_response_id
  "output":[ {"type":"function_call", ...},  // agent 中间步，跳过
             {"type":"message","role":"assistant",
              "content":[{"type":"output_text","text":"最终回答"}]} ] }
```

**`system_prompt` / `user_memory` 的去向（v2 已明确，原 `[待拍板]` 收敛）：** responses 的 `instructions` 字段天然承载系统提示（比 chat/completions 塞 `messages[0]` 干净）→ 把精简后的 `system_prompt` 放 `instructions`；`user_memory` **停止注入**，交给 `X-Hermes-Session-Key=owner` 的长期记忆（避免与 Hermes 记忆重复打架）。

---

## 5. 数据模型变化（ConversationStore 的命运）

`ConversationStore` 原本两个职责：① 存历史 ② 回灌历史给模型。B-2 下 ② 移交 Hermes（responses 服务端状态）。① 怎么处理，取决于 **S-A** 选定的串联机制：

| 方案 | 本地 conversations.json | 回灌历史 | 适配变体 | 评价 |
|---|---|---|---|---|
| (a) 纯透传 | 不再存 messages | 否 | — | 最干净，但丢失本地可观测性 + 回滚困难 |
| (b) 审计镜像 + response_id 映射 | 记 user/assistant（审计）**+ 新增 `conversation_id→latest_response_id`** | 否 | **B-2a** | 需每轮回写最新 response_id；要处理 id 失效降级（§8 S-B）|
| **(c) 审计镜像 + 命名会话透传（倾向）** | 记 user/assistant（审计），**结构几乎不改** | 否 | **B-2b** | `conversation_id` 直接当 `conversation` 字段透传，无需存 response_id；最小改动，**待 S-A 确认命名会话可用** |

推荐路径：**S-A 通过 → (c)/B-2b**；S-A 不通过（命名会话不可用/有坑）→ **(b)/B-2a**。两方案下 `append_turn()` 均保留（落盘审计镜像），`history()` 不再被 `/api/chat` 用于回灌。`new_conversation()` 仍生成 `conversation_id`（B-2b 用作 `conversation` 名 / B-2a 用作映射 key）。`history_limit` 配置项在 B-2 下失效（保留兼容或标记废弃）。

---

## 6. 记忆模型（单用户场景）

> **本项目为单用户**：机器人只为机主一人所用。因此 Hermes 多用户 / 多实例记忆隔离的已知局限（[#6320](https://github.com/NousResearch/hermes-agent/issues/6320)、[#11430](https://github.com/NousResearch/hermes-agent/issues/11430)）**不适用**，隔离顾虑消失，设计大幅简化。
> 本节为单用户结论，**覆盖 §3 表 Session-Key 行、AC-3 / AC-7、R-1 中关于"多设备隔离"的表述**（单用户下均简化或不适用）。

### 6.1 Session-Key 策略：全设备共享"我"的记忆

单用户下，期望的恰恰是**跨设备记忆延续**（客厅设备说的，卧室设备也记得）。因此：

- `X-Hermes-Session-Key` 用**固定机主 key**（如 `owner`），所有硬件共享同一长期记忆 scope —— **不再按 `device_id` 分**。
- `conversation_id` 仍区分不同对话轮次（短期会话，承载方式见 §3）。
- **单 Hermes profile 足够**，无需 per-device profile / 多实例。

> `device_id` 不再用于记忆隔离，但仍保留用于 device_gateway 的设备识别 / 审计镜像 / `device_token` 鉴权。

### 6.2 长期记忆：内置已 always-active，provider 是增强（v2 修正）

v1 原述"长期记忆依赖 memory provider，不配则退化"**不准确**。官方原文：

> "Hermes Agent ships with 8 external memory provider plugins that give the agent persistent, cross-session knowledge **beyond the built-in MEMORY.md and USER.md**." —— 内置 `MEMORY.md` + `USER.md` 记忆 **always active**，无需任何 provider；外部 provider（Honcho 等 8 选 1）是其上的增强（如 Honcho 的动态用户建模 / dialectic 上下文）。

→ 要 B 的长期记忆，**不强依赖 Honcho**：开箱即有基础长期记忆。是否接入 Honcho 降级为"想要更强动态用户建模再加" `[待拍板]` + `[需 spike S-2]`。

### 6.3 安全焦点转移

单用户下安全焦点从"记忆隔离"转为**物理访问控制**（防别人对着我的设备说话）——`device_token` 仍是第一道门；记忆层面无隔离顾虑。

---

## 7. 代码改动清单（B-2）

| # | 文件 | 改动 | 级别 |
|---|---|---|---|
| 1 | `device_gateway/app_core.py` | `ChatService.chat()` 重写：endpoint 从写死 `/chat/completions` 改 `/responses`；payload 改为 `input`/`instructions`/`store`/`previous_response_id`\|`conversation`（**不再调 `build_messages` 拼 history**）；响应解析改为遍历 `output[]` 取 `output_text`，并返回 `(text, response_id)`；注入 `X-Hermes-Session-Key` 头；`timeout` 配置化（新增 `request_timeout`，默认 >60s）；新增开关 `hermes_session_mode`（on=B-2 行为，off=路径 A/DeepSeek 行为，便于回滚）| 核心 |
| 2 | `device_gateway/server.py` | `/api/chat` handler：`device_id`→`session_key`(owner)、`conversation_id`→串联机制；B-2a 从 store 取上轮 `response_id` 传入、拿回新 id 回写；B-2b 把 `conversation_id` 当 `conversation` 透传；按开关决定行为（off 保持原样）| 核心 |
| 3 | `device_gateway/conversation.py` | 保留 `append_turn`（审计镜像）；`history()` B-2 下不参与回灌（加注释说明双模式）；**B-2a 额外**：新增 `conversation_id→latest_response_id` 映射的存取方法 | 小~中（取决 S-A）|
| 4 | `data/llm_config.json` 配置项 | 新增 `request_timeout`、`hermes_session_mode`、`session_key`（默认 `owner`）、串联机制开关 / `instructions` 来源；管理页表单同步 | 小 |
| 5 | `tests/test_device_gateway_core.py`、`tests/test_device_gateway_http.py` | 按 §9 AC 重写（mock transport 断言 `input`/`instructions`/头/取 `output_text`/串联/失效降级/开关）| 测试 |
| 6 | `docs/deployment.md` | §8 接口契约摘要补充 device_gateway→Hermes 调用变化；部署补 Hermes 容器 + `API_SERVER_HOST=0.0.0.0` + state.db 持久化卷 | 文档 |
| 7 | `docs/adr/ADR-001-session-state-ownership.md`（新建）| 记录"会话/记忆状态归属迁移"决策（§11）| 文档 |

> 改动遵循增量验证：先做 #1+#2 的开关骨架（off 时行为与现状完全一致）→ spike 验证（尤其 S-A 定串联机制）→ 打开开关跑通 → 再清理。

---

## 8. Spike 验证清单（写实现代码前必须先实跑 / v2 收敛）

| # | 验证 | 不通过 / 结果的后果 |
|---|---|---|
| ~~S-1~~ | **删除** —— 文档已判负（chat/completions 无状态，见 §2.1），无需实跑 | — |
| **S-A（头号）** | `/v1/responses` 的 `conversation:"<conversation_id>"` 命名会话能否串多轮、是否持久、是否受 100-LRU 影响、与 `X-Hermes-Session-Id`/`Session-Key` 的关系 | 决定走 B-2b/(c) 还是 B-2a/(b)，及 ConversationStore 改不改结构 |
| **S-B** | `previous_response_id` 被 100-LRU 淘汰后再引用，服务端返回什么（错误码 / 静默重开）| 决定 B-2a 的失效降级逻辑（捕获→重开链）；若走 B-2b 命名会话不淘汰则可规避 |
| S-C | responses 多步 / 工具场景下 `output[]` 里 assistant `message` + `output_text` 是否稳定返回最终文本（原 S-4，更精确）| 解析健壮性（需跳过 `function_call` 等中间项）|
| S-2（修正）| 长期记忆：内置 `MEMORY.md`/`USER.md` 已 always-active（开箱即有）；Honcho 等 provider 是动态用户建模增强 | 决定是否值得加 Honcho（非必须）|
| S-5（降级）| Hermes 容器设 `API_SERVER_HOST=0.0.0.0` 后，device_gateway 容器经 docker 网络能否访问 `:8642` | 已基本确认（`API_SERVER_HOST` 配置存在，默认 `127.0.0.1`）；部署时设一下即可 |

> Spike 用 `curl` + 一个临时 Hermes 实例完成，不碰 device_gateway 代码。**S-A 是 B-2 推进的前置门**（定串联机制）；S-B 决定降级逻辑是否必要。

---

## 9. 验收条件（AC）+ 测试骨架

按 test-skeleton-lock：每条 AC 先写一个 FAIL 测试骨架，编码让其转 PASS，编码阶段不改骨架断言。

| AC | 描述 | 测试要点（mock transport）|
|---|---|---|
| AC-1 | `hermes_session_mode=on` 时，`/api/chat` 调 **`/responses`** 且带 `X-Hermes-Session-Key=owner` | 捕获 transport 收到的 URL + headers 断言 |
| AC-2 | on 时请求体用 **`input`（单条当前 prompt）+ `instructions`**，**不含本地 history、不含 user_memory** | 断言 payload 字段（无 `messages`/无回灌）|
| AC-3 | 同 device、不同 `conversation_id` → 两次 `Session-Key` 均为 `owner`；两次串联各自独立（B-2b 不同 `conversation` 名 / B-2a 各自 `response_id` 链）| 两次调用断言头 + 串联字段 |
| AC-4 | 返回给设备的 `answer` 取自 `output[]` 中 assistant `message` 的 `output_text`，**对设备出参结构不变** | 构造含 `function_call`+`message` 的 mock 响应，断言只取最终文本 |
| AC-5 | `request_timeout` 可配置且默认 >60s，透传到 transport | 断言 transport 收到的 timeout |
| AC-6 | 审计镜像：on 时 `append_turn` 仍写入 conversations.json | 断言落盘 |
| AC-7 | `Session-Key` 越界（>256 或含控制符）→ 安全降级（截断 / 拒绝，明确行为）| 边界用例 |
| AC-8 | `hermes_session_mode=off` 时行为与现状完全一致（chat/completions、回灌 history、无 X-Hermes 头）| 回归保护 |
| AC-9（新·B-2a 专属）| `previous_response_id` 失效（mock 返回淘汰错误）→ 捕获并**重开链**（本轮当首轮发），不向设备抛错 | 失效降级用例；若 S-A 选 B-2b 命名会话不淘汰则本条按 S-B 结果调整 |

---

## 10. 风险

| # | 风险 | 处置 |
|---|---|---|
| R-1 | 多用户记忆隔离 Hermes 仍在完善（#6320/#11430）| **单用户不适用**（§6 覆盖）；若未来多用户再评估 profile 方案 |
| R-2 | 长期记忆能力 | 内置 `MEMORY.md`/`USER.md` 已 always-active（§6.2）；Honcho 为可选增强，非前置依赖 |
| R-3 | B-2 串联机制（命名会话语义 / 上限）+ `response_id` 失效降级，文档未明确 | **S-A / S-B 为前置门**；S-A 定变体，S-B 定降级逻辑 |
| R-4 | state.db 在 Hermes 容器，记忆全在那 | 纳入持久化卷 + 备份（类比 device_gateway 的 data 卷）|
| R-5 | 回滚需要本地历史仍在 | 数据模型保留审计镜像（(b)/(c)）+ `hermes_session_mode` 开关 |
| R-6 | agent 变慢、`instructions` 与 Hermes 自身人格叠加 | timeout 调大（§4）；`instructions` 放精简 system，人格主要交 Hermes |
| R-7（新）| 响应解析比 chat/completions 复杂（遍历 `output[]`、跳中间步）| AC-4 用含 `function_call` 的 mock 覆盖；S-C 实测 agent 多步稳定性 |

---

## 11. ADR 草案 —— 会话/记忆状态归属迁移

- **决策**：会话短期上下文 + 长期记忆的存储与拼接，从 device_gateway（`ConversationStore` 回灌）迁移到 Hermes（`/v1/responses` 服务端状态 + 内置/外部记忆）。device_gateway 降级为网关 + 审计镜像。
- **取舍**：得到 Hermes 记忆/skill/学习；代价是记忆落在 Hermes（state.db 成为关键数据）、响应解析变复杂、需选定串联机制（S-A）。
- **替代方案**：① 保持 device_gateway 自管历史（路径 A）——简单但吃不到 Hermes 记忆；② 折中"路径 A + Session-Key 长期记忆"（§2 方案乙）——最小改动拿长期记忆但不接管短期。
- **端点决策**：选 `/v1/responses`（服务端有状态）而非 `/v1/chat/completions`（无状态，B-1 据此出局，见 §2.1）。
- **可逆性**：`hermes_session_mode` 开关 + 本地审计镜像保证可回滚。
- 落地时补全为正式 `docs/adr/ADR-001-session-state-ownership.md`。

---

## 12. 回滚方案

1. `hermes_session_mode=off` → device_gateway 立即回到 chat/completions + 本地回灌 history 行为；
2. `base_url`/`api_key`/`model` 改回 DeepSeek → 完全回到当前线上状态；
3. 本地审计镜像（数据模型 (b)/(c)）保证回滚后历史不丢。
> 切换前 `cat /opt/device_gateway/data/llm_config.json` 备份。

---

## 13. 待拍板决策点

1. ✅ **已定 B-2（`/v1/responses`）**（B-1 证伪，见 §2.1）。**新增待定**：B-2a（`previous_response_id` 链）vs B-2b（`conversation` 命名会话）—— **先 S-A spike**，倾向 B-2b（改动更小）。
2. **记忆 scope**：单 `owner` Session-Key，全设备共享（§6.1，单用户结论）。
3. **是否引入 Honcho**：内置记忆已够基础长期记忆（§6.2）；要动态用户建模再加。**降级为可选**。
4. ✅ **`system_prompt` / `user_memory` 去向**：`system_prompt` → responses 的 `instructions`；`user_memory` 停止注入，交 Session-Key 长期记忆（§4）。
5. **ConversationStore**：(c) 命名会话透传（B-2b，倾向，待 S-A）/ (b) response_id 映射（B-2a）；审计镜像保留。
6. **Hermes 部署位置 + 底层模型**（承接总设计 §11：同机同 Docker 网络 + OpenRouter/DeepSeek/Nous Portal）。
7. **流式落地节奏**：建议分步——先 B-2 非流式验证记忆，再上 SSE 流式（详见 [`hermes-agent-integration-design.md`](./hermes-agent-integration-design.md) §14 语音链路）。

---

## 14. 来源（一手 · 访问日期 2026-06-14）

- 源码 API server：https://github.com/NousResearch/hermes-agent/blob/main/gateway/platforms/api_server.py
- API Server 文档（raw 原文已核）：https://github.com/NousResearch/hermes-agent/blob/main/website/docs/user-guide/features/api-server.md
- 编程集成：https://github.com/NousResearch/hermes-agent/blob/main/website/docs/developer-guide/programmatic-integration.md
- Memory Providers（raw 原文已核）：https://github.com/NousResearch/hermes-agent/blob/main/website/docs/user-guide/features/memory-providers.md
- 记忆隔离局限：[issue #6320](https://github.com/NousResearch/hermes-agent/issues/6320)、[issue #11430](https://github.com/NousResearch/hermes-agent/issues/11430)、[issue #9514](https://github.com/NousResearch/hermes-agent/issues/9514)

**v2 已核实的关键事实（原文摘要）：**
- `/v1/chat/completions`："Stateless — the full conversation is included in each request via the `messages` array."（→ B-1 出局）
- `/v1/responses`："server-side conversation state via `previous_response_id` — the server stores full conversation history ... so multi-turn context is preserved without the client managing it."；请求用 `input`/`instructions`/`store`/`previous_response_id`\|`conversation`；响应遍历 `output[]` 取 `output_text`。
- 存储上限："Max 100 stored responses (LRU eviction)."（仅作用于 `/v1/responses` 的 stored responses；全局 vs per-key 未明确，列 S-A）。
- 记忆：内置 `MEMORY.md`/`USER.md` always-active，8 个外部 provider 是 "beyond the built-in" 的增强，同时仅 1 个外部 provider 可启用。
- `X-Hermes-Session-Key`：在 `/v1/chat/completions`、`/v1/responses`、`/v1/runs` 均支持，threads 给 `AIAgent(gateway_session_key=...)`；≤256 字符。
- `API_SERVER_HOST`：默认 `127.0.0.1`，可改绑定地址（→ S-5 降级）。

> 标 `[需 spike]` 项以实际部署的 Hermes 版本实跑为准，发现与本文不符以实测 / 官方为准并回填。
