import json
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Dict


DEFAULT_CONFIG: Dict[str, str] = {
    "base_url": "http://hermes:8642/v1",
    "model": "hermes-agent",
    "api_key": "",
    "admin_user": "",
    "admin_password": "",
    "admin_path_secret": "",
    "device_token": "",
    "max_prompt_chars": "2000",
    "request_timeout": "120",
    "session_key": "owner",
}


class ConfigStore:
    def __init__(self, path: Path):
        self.path = path

    def get(self) -> Dict[str, str]:
        config = dict(DEFAULT_CONFIG)
        if self.path.exists():
            loaded = json.loads(self.path.read_text(encoding="utf-8"))
            for key in DEFAULT_CONFIG:
                if key in loaded:
                    config[key] = str(loaded[key])
        return config

    def update(self, values: Dict[str, Any]) -> Dict[str, str]:
        config = self.get()
        for key in DEFAULT_CONFIG:
            if key in values:
                config[key] = str(values[key])
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text(
            json.dumps(config, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        return config

    def public_config(self) -> Dict[str, Any]:
        config = self.get()
        return {
            "base_url": config["base_url"],
            "model": config["model"],
            "api_key_set": bool(config["api_key"]),
            "admin_user": config["admin_user"],
            "admin_password_set": bool(config["admin_password"]),
            "admin_path_secret_set": bool(config["admin_path_secret"]),
            "device_token_set": bool(config["device_token"]),
            "max_prompt_chars": int(config["max_prompt_chars"]),
            "request_timeout": int(config["request_timeout"]),
            "session_key": config["session_key"],
        }


def sanitize_session_key(value: str, max_len: int = 256) -> str:
    # X-Hermes-Session-Key goes into an HTTP header and scopes Hermes long-term
    # memory; drop control chars (illegal in header values) and cap at 256.
    raw = (value or "").strip()
    cleaned = "".join(ch for ch in raw if ord(ch) >= 0x20 and ord(ch) != 0x7F)
    return cleaned[:max_len]


def build_responses_payload(
    config: Dict[str, str], prompt: str, conversation_id: str = None
) -> Dict[str, Any]:
    # Thin gateway: Hermes owns persona / memory / model / skills. We send only the
    # user input plus the named `conversation` (= conversation_id) so Hermes keeps
    # short-term multi-turn state server-side.
    payload: Dict[str, Any] = {
        "model": config.get("model", "").strip() or DEFAULT_CONFIG["model"],
        "input": prompt,
        "store": True,
        "stream": False,
    }
    if conversation_id:
        payload["conversation"] = conversation_id
    return payload


def extract_output_text(response: Dict[str, Any]) -> str:
    # /v1/responses returns an output[] array mixing tool steps (function_call /
    # function_call_output) with the final assistant message. The answer is the
    # output_text chunk inside the assistant message item.
    output = response.get("output") if isinstance(response, dict) else None
    if not isinstance(output, list):
        raise RuntimeError("Hermes responses missing 'output' array")
    parts = []
    for item in output:
        if not isinstance(item, dict):
            continue
        if item.get("type") != "message" or item.get("role") != "assistant":
            continue
        for chunk in item.get("content", []) or []:
            if isinstance(chunk, dict) and chunk.get("type") == "output_text":
                text = chunk.get("text")
                if text is not None:
                    parts.append(str(text))
    if not parts:
        raise RuntimeError("Hermes responses missing assistant output_text")
    return "".join(parts)


def _resolve_timeout(config: Dict[str, str]) -> int:
    raw = str(config.get("request_timeout", "") or "").strip()
    try:
        value = int(raw)
    except (TypeError, ValueError):
        return 120
    return value if value > 0 else 120


class UrlLibJsonTransport:
    def post_json(
        self,
        url: str,
        headers: Dict[str, str],
        payload: Dict[str, Any],
        timeout: int,
    ) -> Dict[str, Any]:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        request = urllib.request.Request(
            url,
            data=body,
            headers=headers,
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                data = response.read().decode("utf-8")
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", "replace")
            raise RuntimeError(f"Hermes HTTP {exc.code}: {detail}") from exc
        return json.loads(data)


class ChatService:
    def __init__(self, transport: UrlLibJsonTransport):
        self.transport = transport

    def chat(
        self,
        config: Dict[str, str],
        prompt: str,
        conversation_id: str = None,
    ) -> str:
        # Thin gateway: forward to Hermes /v1/responses. Hermes owns conversation
        # state via the named `conversation` (= conversation_id), plus persona,
        # long-term memory (scoped by X-Hermes-Session-Key), skills and model routing.
        api_key = config.get("api_key", "").strip()
        if not api_key:
            raise ValueError("API Key is required")

        base_url = config.get("base_url", "").strip().rstrip("/")
        if not base_url:
            raise ValueError("base_url is required")

        headers = {
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        }
        session_key = sanitize_session_key(config.get("session_key", ""))
        if session_key:
            headers["X-Hermes-Session-Key"] = session_key

        response = self.transport.post_json(
            f"{base_url}/responses",
            headers,
            build_responses_payload(config, prompt, conversation_id),
            timeout=_resolve_timeout(config),
        )
        return extract_output_text(response)
