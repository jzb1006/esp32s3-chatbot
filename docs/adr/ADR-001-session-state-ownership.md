# ADR-001：会话/记忆状态归属迁移到 Hermes，中间件瘦身为设备网关并改名

> **历史归档说明（2026-06-14）：** 本 ADR 记录 Python `device_gateway` 阶段的架构决策。服务端实现已迁移到 `/Users/jiangzhibin/workspace/chatbot-service-java`，本仓库后续只维护固件相关内容。

| 状态 | 日期 | 决策者 |
|------|------|--------|
| Accepted | 2026-06-14 | jiangzhibin |

## 背景

ESP32-S3 设备经中间件调用大模型。原架构（中间件名 `llm_admin`）直连 DeepSeek `/chat/completions`，自管会话历史（`ConversationStore` 按 `device_id+conversation_id` 存 messages 并每轮回灌），并注入 `system_prompt` / `user_memory`。

引入 NousResearch **Hermes Agent**（自带持久记忆、skill 学习、工具调用、200+ 模型路由）后，需重新定位各层职责。

## 决策

1. **会话/记忆归属迁移到 Hermes**：短期多轮上下文 + 长期记忆 + 人格 + 技能 + 模型选型，全部由 Hermes 持有。中间件不再存储或回灌任何对话历史。
2. **中间件瘦身为纯设备网关**：只负责 `device_token` 鉴权、prompt 限长、`conversation_id` 生成、转发到 Hermes `/v1/responses`、取回答返回设备。`ConversationStore` 整模块删除。
3. **端点选 `/v1/responses`（非 `/v1/chat/completions`）**：经官方文档核实，chat/completions 无状态（必须客户端每轮带全量 messages）；只有 responses 提供服务端会话状态。详见 [`hermes-agent-integration-B-plan.md`](../hermes-agent-integration-B-plan.md) §2。
4. **多轮串联用命名会话（B-2b）**：`conversation_id` 直接透传为 Hermes `conversation` 字段，中间件零会话状态。**依赖部署后 spike S-A 验证**；若命名会话不串多轮，回退 `previous_response_id` 链（B-2a，见 B-plan §5）。
5. **长期记忆 scope 用固定 `owner` key（单用户）**：所有设备共享同一记忆 scope，跨设备记忆延续。
6. **移除 DeepSeek 直连回退**：中间件只有 Hermes 一条路；切换底层模型在 **Hermes 端换 provider**，不在中间件。
7. **中间件改名 `llm_admin` → `device_gateway`**：名实相符（不再是「LLM 管理后台」，而是「设备网关」）。

## 取舍

- **得**：Hermes 的记忆 / skill / 工具 / 模型路由；中间件极薄、易维护；**对设备契约不变**（`/api/chat` 入参出参全不动，固件不烧录）。
- **代价**：
  1. 记忆与会话状态全在 Hermes（`state.db` 成为关键数据，需持久化卷 + 备份）；
  2. 无中间件级回退（Hermes 挂则链路断，用户已接受）；
  3. Hermes 默认暴露**终端命令**工具（安全敏感，须收紧工具集 + `:8642` 绝不放公网）；
  4. 命名会话语义依赖部署后 spike 验证。

## 替代方案

- **路径 A**（保持中间件自管历史）：简单但吃不到 Hermes 记忆。
- **方案乙**（路径 A + `X-Hermes-Session-Key` 长期记忆）：最小改动拿长期记忆但不接管短期。未采纳——用户明确要彻底瘦身、单一后端。

## 可逆性

中间件已极薄。若要回退到直连大模型，需从 git 历史恢复 `_chat_completions` + 历史回灌逻辑（v1 实现）。本决策**倾向不可逆**（用户明确放弃中间件级回退）。

## 验证

- ✅ **已部署上线（2026-06-14）**：device_gateway(8766) + hermes(8642) 同 hermes-net，端到端「设备→网关→hermes→DeepSeek」通，多轮记忆生效（命名会话）。
- ✅ **S-A spike 通过**：命名会话串多轮验证成功（turn2 正确召回 turn1 信息）→ **B-2b 成立，无需回退 B-2a**；S-B（response_id 失效）因 B-2b 不用 response_id 而 N/A。
- ✅ `device_gateway` 单元测试 19 项通过（off 路径已删，纯网关路径全覆盖）。
- ✅ **安全收紧（决策代价③已缓解）**：hermes 禁用 terminal / code_execution / file / computer_use / browser / web / messaging / cronjob / delegation / image_gen，仅保留 memory / skills / todo / vision / tts / session_search / clarify。

## 关联

- [`hermes-agent-integration-design.md`](../hermes-agent-integration-design.md)（总体可行性）
- [`hermes-agent-integration-B-plan.md`](../hermes-agent-integration-B-plan.md)（实现方案）
- [`deployment.md`](../deployment.md)（部署与迁移）
