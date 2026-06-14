#!/usr/bin/env bash
# Hermes Agent 部署后验证脚本
# 目的：决定 device_gateway 维持 B-2b(命名会话) 还是回退 B-2a(previous_response_id 链)。
# 不碰 device_gateway 代码，纯 curl 打 Hermes API server。
#
# 用法:
#   HERMES_URL=http://127.0.0.1:8642/v1 API_KEY=<API_SERVER_KEY> bash docs/hermes-spike.sh
#
# 依赖: curl + python3(用于解析 response id / 美化 JSON)

set -uo pipefail
URL="${HERMES_URL:?set HERMES_URL, e.g. http://127.0.0.1:8642/v1}"
KEY="${API_KEY:?set API_KEY (Hermes API_SERVER_KEY)}"
AUTH="Authorization: Bearer $KEY"
CT="Content-Type: application/json"
hr() { echo "=================================================="; }

# ---------- S-A：命名会话能否串多轮（决定 B-2b 是否成立 = 当前实现）----------
hr; echo "S-A  命名会话串多轮 —— 期望第二轮记得第一轮信息"
CONV="spike-$(date +%s 2>/dev/null || echo conv1)"
echo "[turn1] conversation=$CONV，告诉它一个代号"
curl -s "$URL/responses" -H "$AUTH" -H "$CT" -d '{
  "model":"hermes-agent","store":true,"conversation":"'"$CONV"'",
  "input":"记住：我的设备代号叫 Falcon。只需回复 OK。"}'; echo
echo "[turn2] 同一 conversation 名，问它代号"
curl -s "$URL/responses" -H "$AUTH" -H "$CT" -d '{
  "model":"hermes-agent","store":true,"conversation":"'"$CONV"'",
  "input":"我的设备代号是什么？"}'; echo
echo ">>> 判定: turn2 输出含 'Falcon' → 命名会话可串多轮 → 维持 B-2b（当前实现）。否则回退 B-2a。"

# ---------- S-A'：previous_response_id 链（B-2a 退路的串联方式）----------
hr; echo "S-A' previous_response_id 链 —— 对照验证 B-2a 退路可行性"
RID=$(curl -s "$URL/responses" -H "$AUTH" -H "$CT" -d '{
  "model":"hermes-agent","store":true,
  "input":"记住：今天暗号是 Bluebird。只回 OK。"}' \
  | python3 -c 'import sys,json;print(json.load(sys.stdin).get("id",""))' 2>/dev/null)
echo "first response id = ${RID:-<解析失败>}"
if [ -n "$RID" ]; then
  curl -s "$URL/responses" -H "$AUTH" -H "$CT" -d '{
    "model":"hermes-agent","store":true,"previous_response_id":"'"$RID"'",
    "input":"今天的暗号是什么？"}'; echo
  echo ">>> 判定: 含 'Bluebird' → previous_response_id 链可用（B-2a 退路成立）。"
fi

# ---------- S-B：失效 previous_response_id 的行为（仅 B-2a 需要降级逻辑）----------
hr; echo "S-B  失效 previous_response_id —— 看服务端如何报错"
curl -s -w "\nHTTP %{http_code}\n" "$URL/responses" -H "$AUTH" -H "$CT" -d '{
  "model":"hermes-agent","store":true,"previous_response_id":"resp_nonexistent_xxxxxxxx",
  "input":"hello"}'
echo ">>> 判定: 看 HTTP 码/错误体。若报错 → B-2a 需捕获并重开链(AC-9)；B-2b(命名会话)不涉及此问题。"

# ---------- S-C：output 结构（确认 extract_output_text 解析正确）----------
hr; echo "S-C  output 结构 —— 确认最终文本在 output[].message.content[].output_text"
curl -s "$URL/responses" -H "$AUTH" -H "$CT" -d '{
  "model":"hermes-agent","store":true,"input":"用一句话介绍你自己。"}' \
  | python3 -m json.tool 2>/dev/null || echo "(装 python3 可美化 JSON)"

# ---------- S-5：连通性（device_gateway 容器需能访问此 URL）----------
hr; echo "S-5  连通性 —— 同 docker 网络用服务名(如 http://hermes:8642/v1)，需 API_SERVER_HOST=0.0.0.0"
curl -s -o /dev/null -w "reachable: HTTP %{http_code}\n" "$URL/responses" \
  -H "$AUTH" -H "$CT" -d '{"model":"hermes-agent","input":"ping"}'

hr; echo "完成。关键结论：S-A turn2 含 Falcon? → 维持 B-2b；否则按 B-2a 调整（见 B-plan §5 / ADR-001 决策4）。"
