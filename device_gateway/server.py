import argparse
import base64
import hmac
import html
import json
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Dict

from device_gateway import app_core


DEFAULT_CONFIG_PATH = Path(__file__).resolve().parents[1] / "data" / "llm_config.json"


def read_json(handler: BaseHTTPRequestHandler) -> Dict[str, Any]:
    length = int(handler.headers.get("Content-Length", "0") or "0")
    if length <= 0:
        return {}
    data = handler.rfile.read(length).decode("utf-8")
    return json.loads(data)


def parse_chat_request(config: Dict[str, str], values: Dict[str, Any]):
    prompt = str(values.get("prompt", "")).strip()
    if not prompt:
        return None, {"status": 400, "body": {"error": "prompt is required"}}
    max_prompt_chars = int(config["max_prompt_chars"])
    if len(prompt) > max_prompt_chars:
        return None, {"status": 413, "body": {"error": "prompt too long"}}
    device_id = str(values.get("device_id", "default-device")).strip()
    if not device_id:
        device_id = "default-device"
    conversation_id = str(values.get("conversation_id", "")).strip()
    if not conversation_id:
        conversation_id = uuid.uuid4().hex
    return (device_id, conversation_id, prompt), None


def sse_event(event: str, data: Dict[str, Any]) -> bytes:
    payload = json.dumps(data, ensure_ascii=False)
    return f"event: {event}\ndata: {payload}\n\n".encode("utf-8")


def token_status(expected: str, actual: str) -> int:
    if not expected:
        return 200
    if not actual:
        return 401
    if actual != expected:
        return 403
    return 200


def check_basic_auth(
    header_value: str, expected_user: str, expected_password: str
) -> bool:
    # HTTP Basic Auth verification with constant-time comparison.
    if not header_value.startswith("Basic "):
        return False
    encoded = header_value[len("Basic "):].strip()
    try:
        decoded = base64.b64decode(encoded).decode("utf-8")
    except Exception:
        return False
    if ":" not in decoded:
        return False
    user, password = decoded.split(":", 1)
    user_ok = hmac.compare_digest(
        user.encode("utf-8"), expected_user.encode("utf-8")
    )
    password_ok = hmac.compare_digest(
        password.encode("utf-8"), expected_password.encode("utf-8")
    )
    return user_ok and password_ok


def admin_base_path(config: Dict[str, Any]) -> str:
    # Admin page lives under a hard-to-guess random path segment when configured.
    secret = config.get("admin_path_secret", "")
    if secret:
        return "/admin/" + secret
    return "/admin"


def make_handler(
    store: app_core.ConfigStore,
    chat_service: app_core.ChatService,
):
    class LlmAdminHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            config = store.get()
            path = self.path.split("?", 1)[0]
            base = admin_base_path(config)
            if path == base:
                if not self.require_admin(config):
                    return
                self.send_html(render_admin_page(store.public_config()))
                return
            if path == base + "/api/config":
                if not self.require_admin(config):
                    return
                self.send_json(200, store.public_config())
                return
            self.send_json(404, {"error": "not found"})

        def do_POST(self):
            config = store.get()
            path = self.path.split("?", 1)[0]
            base = admin_base_path(config)
            if path == base + "/api/config":
                if not self.require_admin(config):
                    return
                values = read_json(self)
                store.update(values)
                self.send_json(200, store.public_config())
                return
            if path == "/api/conversations/new":
                if not self.require_device(config):
                    return
                values = read_json(self)
                device_id = str(values.get("device_id", "default-device")).strip()
                if not device_id:
                    device_id = "default-device"
                conversation_id = uuid.uuid4().hex
                self.send_json(
                    200,
                    {
                        "device_id": device_id,
                        "conversation_id": conversation_id,
                    },
                )
                return
            if path == "/api/voice/chat":
                if not self.require_device(config):
                    return
                self.send_json(501, {"error": "voice_not_ready"})
                return
            if path == "/api/chat/stream":
                if not self.require_device(config):
                    return
                parsed, error = parse_chat_request(config, read_json(self))
                if error:
                    self.send_json(error["status"], error["body"])
                    return
                device_id, conversation_id, prompt = parsed
                self.start_sse()
                self.wfile.write(
                    sse_event(
                        "conversation",
                        {
                            "device_id": device_id,
                            "conversation_id": conversation_id,
                        },
                    )
                )
                try:
                    for chunk in chat_service.stream_chat(
                        config, prompt, conversation_id=conversation_id
                    ):
                        self.wfile.write(chunk)
                except Exception as exc:
                    self.wfile.write(sse_event("error", {"error": str(exc)}))
                return
            if path == "/api/chat":
                if not self.require_device(config):
                    return
                parsed, error = parse_chat_request(config, read_json(self))
                if error:
                    self.send_json(error["status"], error["body"])
                    return
                device_id, conversation_id, prompt = parsed
                # Thin gateway: forward to Hermes; Hermes owns conversation state via
                # the named conversation (= conversation_id). No local history/mirror.
                try:
                    answer = chat_service.chat(
                        config, prompt, conversation_id=conversation_id
                    )
                except Exception as exc:
                    self.send_json(500, {"error": str(exc)})
                    return
                self.send_json(
                    200,
                    {
                        "device_id": device_id,
                        "conversation_id": conversation_id,
                        "answer": answer,
                    },
                )
                return
            self.send_json(404, {"error": "not found"})

        def log_message(self, format, *args):
            return

        def send_json(self, status: int, body: Dict[str, Any]):
            payload = json.dumps(body, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def send_html(self, body: str):
            payload = body.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def start_sse(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()

        def send_auth_challenge(self):
            # 401 + WWW-Authenticate makes the browser pop the username/password dialog.
            payload = json.dumps({"error": "authentication required"}).encode("utf-8")
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Basic realm="Device Gateway"')
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def require_admin(self, config: Dict[str, Any]) -> bool:
            expected_user = config.get("admin_user", "")
            expected_password = config.get("admin_password", "")
            # No admin password set yet => treat as uninitialized and allow,
            # so the very first configuration can be done.
            if not expected_password:
                return True
            auth = self.headers.get("Authorization", "")
            if check_basic_auth(auth, expected_user, expected_password):
                return True
            self.send_auth_challenge()
            return False

        def require_device(self, config: Dict[str, Any]) -> bool:
            status = token_status(
                config["device_token"],
                self.headers.get("X-Device-Token", ""),
            )
            if status != 200:
                self.send_json(status, {"error": "device token required"})
                return False
            return True

    return LlmAdminHandler


def render_admin_page(config: Dict[str, Any]) -> str:
    api_key_note = "已配置" if config.get("api_key_set") else "未配置"
    password_note = "已配置" if config.get("admin_password_set") else "未配置"
    secret_note = "已配置" if config.get("admin_path_secret_set") else "未配置"
    device_token_note = "已配置" if config.get("device_token_set") else "未配置"
    return f"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32-S3 设备网关</title>
  <style>
    body {{ font-family: -apple-system, BlinkMacSystemFont, Helvetica, Arial, sans-serif; margin: 24px; color: #1f2933; }}
    main {{ max-width: 760px; margin: 0 auto; }}
    label {{ display: block; margin: 16px 0 6px; font-weight: 600; }}
    input, textarea {{ width: 100%; box-sizing: border-box; padding: 10px; font: inherit; border: 1px solid #c9d1d9; border-radius: 6px; }}
    textarea {{ min-height: 90px; }}
    button {{ margin-top: 18px; padding: 10px 14px; border: 0; border-radius: 6px; background: #0969da; color: #fff; font: inherit; }}
    pre {{ white-space: pre-wrap; background: #f6f8fa; padding: 12px; border-radius: 6px; }}
    .hint {{ color: #57606a; font-size: 13px; font-weight: 400; }}
  </style>
</head>
<body>
<main>
  <h1>ESP32-S3 设备网关</h1>
  <p>API Key：{html.escape(api_key_note)} / 管理密码：{html.escape(password_note)} / 管理路径：{html.escape(secret_note)} / 设备 Token：{html.escape(device_token_note)}</p>
  <p class="hint">瘦网关模式：人格 / 记忆 / 模型选型 / 技能均在 Hermes 端配置，本后台只管设备接入与转发。</p>
  <label>大模型 URL <span class="hint">（Hermes 地址，如 http://hermes:8642/v1）</span></label>
  <input id="base_url" value="{html.escape(str(config.get("base_url", "")))}">
  <label>模型名 <span class="hint">（Hermes 端决定，通常填 hermes-agent）</span></label>
  <input id="model" value="{html.escape(str(config.get("model", "")))}">
  <label>API Key <span class="hint">（Hermes 的 API_SERVER_KEY；真正的大模型 key 在 Hermes 端配）</span></label>
  <input id="api_key" type="password" placeholder="留空则不修改">
  <label>记忆 Key <span class="hint">（长期记忆 scope；单用户填 owner，全设备共享）</span></label>
  <input id="session_key" value="{html.escape(str(config.get("session_key", "")))}">
  <label>请求超时（秒） <span class="hint">（Agent 多步较慢，建议 120~180）</span></label>
  <input id="request_timeout" type="number" min="1" value="{html.escape(str(config.get("request_timeout", 60)))}">
  <label>最大 prompt 字符数</label>
  <input id="max_prompt_chars" type="number" min="1" value="{html.escape(str(config.get("max_prompt_chars", 2000)))}">
  <label>管理账号</label>
  <input id="admin_user" value="{html.escape(str(config.get("admin_user", "")))}" placeholder="登录用户名">
  <label>管理密码</label>
  <input id="admin_password" type="password" placeholder="留空则不修改">
  <label>管理路径随机串 <span class="hint">（改后需用新地址 /admin/&lt;新串&gt; 重新登录）</span></label>
  <input id="admin_path_secret" placeholder="留空则不修改">
  <label>设备 Token</label>
  <input id="device_token" type="password" placeholder="留空则不修改">
  <button onclick="saveConfig()">保存配置</button>
  <label>测试设备 ID</label>
  <input id="device_id" value="browser-test-device">
  <label>测试会话 ID</label>
  <input id="conversation_id" placeholder="留空则自动创建">
  <label>测试设备 Token</label>
  <input id="test_device_token" type="password" placeholder="如已配置设备 Token，则必须填写">
  <label>测试问题</label>
  <textarea id="prompt">你好，用一句话介绍你自己。</textarea>
  <button onclick="sendChat()">测试聊天</button>
  <pre id="result"></pre>
</main>
<script>
async function saveConfig() {{
  const payload = {{
    base_url: document.getElementById('base_url').value,
    model: document.getElementById('model').value,
    session_key: document.getElementById('session_key').value,
    request_timeout: document.getElementById('request_timeout').value,
    max_prompt_chars: document.getElementById('max_prompt_chars').value
  }};
  const key = document.getElementById('api_key').value;
  if (key) payload.api_key = key;
  const adminUser = document.getElementById('admin_user').value;
  if (adminUser) payload.admin_user = adminUser;
  const adminPassword = document.getElementById('admin_password').value;
  if (adminPassword) payload.admin_password = adminPassword;
  const adminSecret = document.getElementById('admin_path_secret').value;
  if (adminSecret) payload.admin_path_secret = adminSecret;
  const deviceToken = document.getElementById('device_token').value;
  if (deviceToken) payload.device_token = deviceToken;
  // location.pathname 是当前管理页地址 /admin/<secret>，配置接口在其下，
  // 浏览器会对该前缀自动带上已登录的 Basic Auth 凭据。
  const res = await fetch(location.pathname + '/api/config', {{
    method: 'POST',
    headers: {{ 'Content-Type': 'application/json' }},
    body: JSON.stringify(payload)
  }});
  document.getElementById('result').textContent = JSON.stringify(await res.json(), null, 2);
}}
async function sendChat() {{
  const payload = {{
    device_id: document.getElementById('device_id').value,
    conversation_id: document.getElementById('conversation_id').value,
    prompt: document.getElementById('prompt').value
  }};
  if (!payload.conversation_id) delete payload.conversation_id;
  const res = await fetch('/api/chat', {{
    method: 'POST',
    headers: {{
      'Content-Type': 'application/json',
      'X-Device-Token': document.getElementById('test_device_token').value
    }},
    body: JSON.stringify(payload)
  }});
  const body = await res.json();
  if (body.conversation_id) document.getElementById('conversation_id').value = body.conversation_id;
  document.getElementById('result').textContent = JSON.stringify(body, null, 2);
}}
</script>
</body>
</html>
"""


def run(host: str, port: int, config_path: Path):
    store = app_core.ConfigStore(config_path)
    chat_service = app_core.ChatService(app_core.UrlLibJsonTransport())
    httpd = ThreadingHTTPServer(
        (host, port),
        make_handler(store, chat_service),
    )
    print(f"Device gateway listening on http://{host}:{port}")
    print(f"Config file: {config_path}")
    httpd.serve_forever()


def main():
    parser = argparse.ArgumentParser(description="ESP32-S3 device gateway (ESP32 to Hermes)")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8766)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG_PATH)
    args = parser.parse_args()
    run(args.host, args.port, args.config)


if __name__ == "__main__":
    main()
