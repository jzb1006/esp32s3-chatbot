import json
import tempfile
import unittest
import base64
from pathlib import Path
from unittest import mock

from llm_admin import app_core, server
from llm_admin.conversation import ConversationStore


class FakeChatService:
    def __init__(self):
        self.calls = []

    def chat(self, config, prompt, history=None):
        self.calls.append((config, prompt, history or []))
        return f"answer:{prompt}"


class HttpHandlerTest(unittest.TestCase):
    def make_handler(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        store = server.app_core.ConfigStore(Path(tmp.name) / "config.json")
        conversation_store = ConversationStore(Path(tmp.name) / "conversations.json")
        chat_service = FakeChatService()
        handler_cls = server.make_handler(store, chat_service, conversation_store)
        return handler_cls, store, chat_service, conversation_store

    def call_json(self, handler_cls, method, path, payload=None, headers=None):
        body = b""
        headers = dict(headers or {})
        if payload is not None:
            body = json.dumps(payload).encode("utf-8")
            headers["Content-Length"] = str(len(body))

        handler = object.__new__(handler_cls)
        handler.rfile = mock.Mock()
        handler.rfile.read.return_value = body
        handler.wfile = mock.Mock()
        handler.headers = headers
        handler.path = path
        handler.command = method
        handler.request_version = "HTTP/1.1"
        handler.responses = []

        def send_response(code, message=None):
            handler.responses.append(("status", code))

        def send_header(name, value):
            handler.responses.append(("header", name, value))

        def end_headers():
            handler.responses.append(("end",))

        handler.send_response = send_response
        handler.send_header = send_header
        handler.end_headers = end_headers

        if method == "GET":
            handler.do_GET()
        elif method == "POST":
            handler.do_POST()
        else:
            raise AssertionError(method)

        response_body = handler.wfile.write.call_args[0][0]
        return handler.responses, json.loads(response_body.decode("utf-8"))

    def test_get_config_returns_public_config(self):
        handler_cls, store, _, _ = self.make_handler()
        store.update({"api_key": "sk-test", "model": "deepseek-test"})

        responses, body = self.call_json(handler_cls, "GET", "/admin/api/config")

        self.assertIn(("status", 200), responses)
        self.assertEqual(body["model"], "deepseek-test")
        self.assertTrue(body["api_key_set"])
        self.assertNotIn("api_key", body)

    def test_post_config_updates_known_fields(self):
        handler_cls, store, _, _ = self.make_handler()

        responses, body = self.call_json(
            handler_cls,
            "POST",
            "/admin/api/config",
            {
                "base_url": "https://api.example.com",
                "api_key": "sk-test",
                "ignored": "value",
            },
        )

        self.assertIn(("status", 200), responses)
        self.assertEqual(body["base_url"], "https://api.example.com")
        self.assertTrue(body["api_key_set"])
        self.assertEqual(store.get()["api_key"], "sk-test")
        self.assertNotIn("ignored", store.get())

    def test_admin_basic_auth_and_random_path_protect_admin(self):
        handler_cls, store, _, _ = self.make_handler()
        store.update(
            {
                "admin_user": "boss",
                "admin_password": "pw-secret",
                "admin_path_secret": "rndpath",
            }
        )
        base = "/admin/rndpath"

        # 正确路径但无凭据 -> 401 + 浏览器弹框挑战
        responses, body = self.call_json(handler_cls, "GET", base + "/api/config")
        self.assertIn(("status", 401), responses)
        self.assertIn(
            ("header", "WWW-Authenticate", 'Basic realm="LLM Admin"'), responses
        )

        # 正确路径 + 正确账号密码 -> 200
        good = base64.b64encode(b"boss:pw-secret").decode("ascii")
        responses, body = self.call_json(
            handler_cls,
            "GET",
            base + "/api/config",
            headers={"Authorization": f"Basic {good}"},
        )
        self.assertIn(("status", 200), responses)

        # 正确路径 + 错误密码 -> 401
        bad = base64.b64encode(b"boss:wrong").decode("ascii")
        responses, body = self.call_json(
            handler_cls,
            "GET",
            base + "/api/config",
            headers={"Authorization": f"Basic {bad}"},
        )
        self.assertIn(("status", 401), responses)

        # 猜错随机路径 -> 404（即使带正确凭据，也不暴露后台位置）
        responses, body = self.call_json(
            handler_cls,
            "GET",
            "/admin/wrongpath/api/config",
            headers={"Authorization": f"Basic {good}"},
        )
        self.assertIn(("status", 404), responses)

        # 旧的 /admin 裸路径 -> 404
        responses, body = self.call_json(handler_cls, "GET", "/admin")
        self.assertIn(("status", 404), responses)

    def test_device_token_protects_chat_when_set(self):
        handler_cls, store, _, _ = self.make_handler()
        store.update({"api_key": "sk-test", "device_token": "device-secret"})

        responses, body = self.call_json(
            handler_cls,
            "POST",
            "/api/chat",
            {"prompt": "你好"},
        )
        self.assertIn(("status", 401), responses)

        responses, body = self.call_json(
            handler_cls,
            "POST",
            "/api/chat",
            {"prompt": "你好"},
            headers={"X-Device-Token": "wrong"},
        )
        self.assertIn(("status", 403), responses)

    def test_post_chat_delegates_prompt_to_service(self):
        handler_cls, store, chat_service, conversation_store = self.make_handler()
        store.update({"api_key": "sk-test"})

        responses, body = self.call_json(
            handler_cls,
            "POST",
            "/api/chat",
            {"prompt": "你好"},
        )

        self.assertIn(("status", 200), responses)
        self.assertEqual(body["device_id"], "default-device")
        self.assertTrue(body["conversation_id"])
        self.assertEqual(body["answer"], "answer:你好")
        self.assertEqual(chat_service.calls[0][1], "你好")
        self.assertEqual(chat_service.calls[0][0]["api_key"], "sk-test")
        self.assertEqual(
            conversation_store.history("default-device", body["conversation_id"], limit=8),
            [
                {"role": "user", "content": "你好"},
                {"role": "assistant", "content": "answer:你好"},
            ],
        )

    def test_post_chat_reuses_history_for_same_conversation(self):
        handler_cls, store, chat_service, conversation_store = self.make_handler()
        store.update({"api_key": "sk-test", "history_limit": 8})
        conversation_id = conversation_store.new_conversation("device-a")
        conversation_store.append_turn("device-a", conversation_id, "我叫什么？", "你叫小江。")

        responses, body = self.call_json(
            handler_cls,
            "POST",
            "/api/chat",
            {
                "device_id": "device-a",
                "conversation_id": conversation_id,
                "prompt": "再说一次",
            },
        )

        self.assertIn(("status", 200), responses)
        self.assertEqual(body["conversation_id"], conversation_id)
        self.assertEqual(
            chat_service.calls[0][2],
            [
                {"role": "user", "content": "我叫什么？"},
                {"role": "assistant", "content": "你叫小江。"},
            ],
        )

    def test_prompt_length_limit_is_enforced(self):
        handler_cls, store, _, _ = self.make_handler()
        store.update({"api_key": "sk-test", "max_prompt_chars": 3})

        responses, body = self.call_json(
            handler_cls,
            "POST",
            "/api/chat",
            {"prompt": "超过三字"},
        )

        self.assertIn(("status", 413), responses)

    def test_new_conversation_endpoint_requires_device_token_and_returns_id(self):
        handler_cls, store, _, _ = self.make_handler()
        store.update({"device_token": "device-secret"})

        responses, body = self.call_json(
            handler_cls,
            "POST",
            "/api/conversations/new",
            {"device_id": "device-a"},
            headers={"X-Device-Token": "device-secret"},
        )

        self.assertIn(("status", 200), responses)
        self.assertEqual(body["device_id"], "device-a")
        self.assertTrue(body["conversation_id"])

    def test_voice_chat_placeholder_requires_device_token(self):
        handler_cls, store, _, _ = self.make_handler()
        store.update({"device_token": "device-secret"})

        responses, body = self.call_json(
            handler_cls,
            "POST",
            "/api/voice/chat",
            {"device_id": "device-a"},
            headers={"X-Device-Token": "device-secret"},
        )

        self.assertIn(("status", 501), responses)
        self.assertEqual(body, {"error": "voice_not_ready"})


if __name__ == "__main__":
    unittest.main()
