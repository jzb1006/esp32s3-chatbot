import json
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Dict, List


DEFAULT_CONFIG: Dict[str, str] = {
    "base_url": "https://api.deepseek.com",
    "model": "deepseek-chat",
    "api_key": "",
    "admin_user": "",
    "admin_password": "",
    "admin_path_secret": "",
    "device_token": "",
    "max_prompt_chars": "2000",
    "history_limit": "8",
    "system_prompt": "",
    "user_memory": "",
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
            "system_prompt": config["system_prompt"],
            "user_memory": config["user_memory"],
            "api_key_set": bool(config["api_key"]),
            "admin_user": config["admin_user"],
            "admin_password_set": bool(config["admin_password"]),
            "admin_path_secret_set": bool(config["admin_path_secret"]),
            "device_token_set": bool(config["device_token"]),
            "max_prompt_chars": int(config["max_prompt_chars"]),
            "history_limit": int(config["history_limit"]),
        }


def build_messages(
    config: Dict[str, str],
    prompt: str,
    history: List[Dict[str, str]] = None,
) -> List[Dict[str, str]]:
    messages: List[Dict[str, str]] = []
    system_prompt = config.get("system_prompt", "").strip()
    user_memory = config.get("user_memory", "").strip()
    if system_prompt:
        messages.append({"role": "system", "content": system_prompt})
    if user_memory:
        messages.append({"role": "system", "content": f"用户记忆：{user_memory}"})
    messages.extend(history or [])
    messages.append({"role": "user", "content": prompt})
    return messages


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
            raise RuntimeError(f"LLM HTTP {exc.code}: {detail}") from exc
        return json.loads(data)


class ChatService:
    def __init__(self, transport: UrlLibJsonTransport):
        self.transport = transport

    def chat(
        self,
        config: Dict[str, str],
        prompt: str,
        history: List[Dict[str, str]] = None,
    ) -> str:
        api_key = config.get("api_key", "").strip()
        if not api_key:
            raise ValueError("API Key is required")

        base_url = config.get("base_url", "").strip().rstrip("/")
        if not base_url:
            raise ValueError("base_url is required")

        payload = {
            "model": config.get("model", "").strip() or DEFAULT_CONFIG["model"],
            "messages": build_messages(config, prompt, history),
            "stream": False,
        }
        response = self.transport.post_json(
            f"{base_url}/chat/completions",
            {
                "Authorization": f"Bearer {api_key}",
                "Content-Type": "application/json",
            },
            payload,
            timeout=60,
        )
        try:
            return str(response["choices"][0]["message"]["content"])
        except (KeyError, IndexError, TypeError) as exc:
            raise RuntimeError("LLM response missing choices[0].message.content") from exc
