import json
import tempfile
import unittest
from pathlib import Path

from llm_admin import app_core


class ConfigStoreTest(unittest.TestCase):
    def test_defaults_are_safe_for_device_reads(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = app_core.ConfigStore(Path(tmp) / "config.json")

            public_config = store.public_config()

        self.assertEqual(public_config["base_url"], "https://api.deepseek.com")
        self.assertEqual(public_config["model"], "deepseek-chat")
        self.assertEqual(public_config["system_prompt"], "")
        self.assertEqual(public_config["user_memory"], "")
        self.assertEqual(public_config["api_key_set"], False)
        self.assertEqual(public_config["admin_user"], "")
        self.assertEqual(public_config["admin_password_set"], False)
        self.assertEqual(public_config["admin_path_secret_set"], False)
        self.assertEqual(public_config["device_token_set"], False)
        self.assertEqual(public_config["max_prompt_chars"], 2000)
        self.assertEqual(public_config["history_limit"], 8)
        self.assertNotIn("api_key", public_config)
        self.assertNotIn("admin_password", public_config)
        self.assertNotIn("admin_path_secret", public_config)
        self.assertNotIn("device_token", public_config)

    def test_update_merges_known_fields_and_persists(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "config.json"
            store = app_core.ConfigStore(path)

            updated = store.update(
                {
                    "base_url": "https://api.example.com",
                    "model": "deepseek-test",
                    "api_key": "sk-test",
                    "admin_user": "boss",
                    "admin_password": "pw-secret",
                    "admin_path_secret": "rand-path",
                    "device_token": "device-secret",
                    "max_prompt_chars": 128,
                    "history_limit": 4,
                    "system_prompt": "你是设备助手",
                    "user_memory": "用户喜欢简洁回答",
                    "ignored": "value",
                }
            )
            reloaded = app_core.ConfigStore(path)

            self.assertEqual(updated["base_url"], "https://api.example.com")
            self.assertEqual(reloaded.get()["api_key"], "sk-test")
            self.assertEqual(reloaded.get()["admin_user"], "boss")
            self.assertEqual(reloaded.get()["admin_password"], "pw-secret")
            self.assertEqual(reloaded.get()["admin_path_secret"], "rand-path")
            self.assertEqual(reloaded.get()["device_token"], "device-secret")
            self.assertEqual(reloaded.get()["max_prompt_chars"], "128")
            self.assertEqual(reloaded.get()["history_limit"], "4")
            self.assertNotIn("ignored", reloaded.get())
            self.assertTrue(reloaded.public_config()["api_key_set"])
            self.assertEqual(reloaded.public_config()["admin_user"], "boss")
            self.assertTrue(reloaded.public_config()["admin_password_set"])
            self.assertTrue(reloaded.public_config()["admin_path_secret_set"])
            self.assertTrue(reloaded.public_config()["device_token_set"])
            self.assertNotIn("api_key", reloaded.public_config())
            self.assertNotIn("admin_password", reloaded.public_config())
            self.assertNotIn("admin_path_secret", reloaded.public_config())
            self.assertNotIn("device_token", reloaded.public_config())


class ChatServiceTest(unittest.TestCase):
    def test_build_messages_includes_prompt_memory_history_and_user_text(self):
        config = {
            "system_prompt": "你是设备助手",
            "user_memory": "用户叫小江",
        }
        history = [
            {"role": "user", "content": "我叫什么？"},
            {"role": "assistant", "content": "你叫小江。"},
        ]

        messages = app_core.build_messages(config, "今天帮我做什么？", history)

        self.assertEqual(
            messages,
            [
                {"role": "system", "content": "你是设备助手"},
                {"role": "system", "content": "用户记忆：用户叫小江"},
                {"role": "user", "content": "我叫什么？"},
                {"role": "assistant", "content": "你叫小江。"},
                {"role": "user", "content": "今天帮我做什么？"},
            ],
        )

    def test_chat_requires_api_key(self):
        class UnusedTransport:
            def post_json(self, url, headers, payload, timeout):
                raise AssertionError("transport should not be called")

        service = app_core.ChatService(UnusedTransport())

        with self.assertRaisesRegex(ValueError, "API Key"):
            service.chat(
                {
                    "base_url": "https://api.deepseek.com",
                    "model": "deepseek-chat",
                    "api_key": "",
                    "system_prompt": "",
                    "user_memory": "",
                },
                "hello",
            )

    def test_chat_uses_openai_compatible_payload(self):
        class FakeTransport:
            def __init__(self):
                self.calls = []

            def post_json(self, url, headers, payload, timeout):
                self.calls.append((url, headers, payload, timeout))
                return {
                    "choices": [
                        {"message": {"content": "你好，我是 DeepSeek。"}},
                    ]
                }

        transport = FakeTransport()
        service = app_core.ChatService(transport)

        answer = service.chat(
            {
                "base_url": "https://api.deepseek.com/",
                "model": "deepseek-chat",
                "api_key": "sk-test",
                "system_prompt": "短回答",
                "user_memory": "",
            },
            "hello",
        )

        self.assertEqual(answer, "你好，我是 DeepSeek。")
        url, headers, payload, timeout = transport.calls[0]
        self.assertEqual(url, "https://api.deepseek.com/chat/completions")
        self.assertEqual(headers["Authorization"], "Bearer sk-test")
        self.assertEqual(payload["model"], "deepseek-chat")
        self.assertEqual(payload["messages"][0], {"role": "system", "content": "短回答"})
        self.assertEqual(payload["messages"][-1], {"role": "user", "content": "hello"})
        self.assertEqual(timeout, 60)


if __name__ == "__main__":
    unittest.main()
