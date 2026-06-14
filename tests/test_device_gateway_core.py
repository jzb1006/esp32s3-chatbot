import tempfile
import unittest
from pathlib import Path

from device_gateway import app_core


class ConfigStoreTest(unittest.TestCase):
    def test_defaults_are_safe_for_device_reads(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = app_core.ConfigStore(Path(tmp) / "config.json")
            public_config = store.public_config()

        self.assertEqual(public_config["base_url"], "http://hermes:8642/v1")
        self.assertEqual(public_config["model"], "hermes-agent")
        self.assertEqual(public_config["api_key_set"], False)
        self.assertEqual(public_config["admin_user"], "")
        self.assertEqual(public_config["admin_password_set"], False)
        self.assertEqual(public_config["admin_path_secret_set"], False)
        self.assertEqual(public_config["device_token_set"], False)
        self.assertEqual(public_config["max_prompt_chars"], 2000)
        self.assertEqual(public_config["request_timeout"], 120)
        self.assertEqual(public_config["session_key"], "owner")
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
                    "base_url": "http://hermes:8642/v1",
                    "model": "hermes-agent",
                    "api_key": "srv-key",
                    "admin_user": "boss",
                    "admin_password": "pw-secret",
                    "admin_path_secret": "rand-path",
                    "device_token": "device-secret",
                    "max_prompt_chars": 128,
                    "request_timeout": 150,
                    "session_key": "owner",
                    "ignored": "value",
                }
            )
            reloaded = app_core.ConfigStore(path)

            self.assertEqual(updated["base_url"], "http://hermes:8642/v1")
            self.assertEqual(reloaded.get()["api_key"], "srv-key")
            self.assertEqual(reloaded.get()["admin_user"], "boss")
            self.assertEqual(reloaded.get()["device_token"], "device-secret")
            self.assertEqual(reloaded.get()["request_timeout"], "150")
            self.assertEqual(reloaded.get()["session_key"], "owner")
            self.assertNotIn("ignored", reloaded.get())
            self.assertTrue(reloaded.public_config()["api_key_set"])
            self.assertTrue(reloaded.public_config()["device_token_set"])
            self.assertNotIn("api_key", reloaded.public_config())
            self.assertNotIn("admin_password", reloaded.public_config())


class ChatServiceTest(unittest.TestCase):
    class FakeTransport:
        def __init__(self, response):
            self.response = response
            self.calls = []
            self.stream_calls = []

        def post_json(self, url, headers, payload, timeout):
            self.calls.append((url, headers, payload, timeout))
            return self.response

        def post_stream(self, url, headers, payload, timeout):
            self.stream_calls.append((url, headers, payload, timeout))
            return [
                b"event: response.output_text.delta\n",
                'data: {"delta":"你"}\n\n'.encode("utf-8"),
            ]

    def _config(self, **overrides):
        config = {
            "base_url": "http://hermes:8642/v1",
            "model": "hermes-agent",
            "api_key": "srv-key",
            "session_key": "owner",
            "request_timeout": "150",
        }
        config.update(overrides)
        return config

    def test_chat_requires_api_key(self):
        class UnusedTransport:
            def post_json(self, url, headers, payload, timeout):
                raise AssertionError("transport should not be called")

        service = app_core.ChatService(UnusedTransport())
        with self.assertRaisesRegex(ValueError, "API Key"):
            service.chat(
                {"base_url": "http://hermes:8642/v1", "api_key": ""}, "hello"
            )

    def test_chat_forwards_to_responses_with_session_key_and_conversation(self):
        transport = self.FakeTransport(
            {
                "id": "resp_123",
                "output": [
                    {"type": "function_call", "name": "search"},
                    {
                        "type": "message",
                        "role": "assistant",
                        "content": [
                            {"type": "output_text", "text": "你好，我是 Hermes。"}
                        ],
                    },
                ],
            }
        )
        service = app_core.ChatService(transport)

        answer = service.chat(self._config(), "hello", conversation_id="conv-1")

        self.assertEqual(answer, "你好，我是 Hermes。")
        url, headers, payload, timeout = transport.calls[0]
        self.assertEqual(url, "http://hermes:8642/v1/responses")
        self.assertEqual(headers["Authorization"], "Bearer srv-key")
        self.assertEqual(headers["X-Hermes-Session-Key"], "owner")
        self.assertEqual(payload["input"], "hello")
        self.assertEqual(payload["conversation"], "conv-1")
        self.assertTrue(payload["store"])
        self.assertFalse(payload["stream"])
        # thin: persona/memory/history live in Hermes, not injected by middleware
        self.assertNotIn("messages", payload)
        self.assertNotIn("instructions", payload)
        self.assertEqual(timeout, 150)

    def test_stream_chat_forwards_to_responses_with_stream_enabled(self):
        transport = self.FakeTransport({})
        service = app_core.ChatService(transport)

        chunks = list(service.stream_chat(self._config(), "hello", conversation_id="conv-1"))

        self.assertEqual(
            chunks,
            [
                b"event: response.output_text.delta\n",
                'data: {"delta":"你"}\n\n'.encode("utf-8"),
            ],
        )
        url, headers, payload, timeout = transport.stream_calls[0]
        self.assertEqual(url, "http://hermes:8642/v1/responses")
        self.assertEqual(headers["Authorization"], "Bearer srv-key")
        self.assertEqual(headers["X-Hermes-Session-Key"], "owner")
        self.assertEqual(payload["input"], "hello")
        self.assertEqual(payload["conversation"], "conv-1")
        self.assertTrue(payload["store"])
        self.assertTrue(payload["stream"])
        self.assertNotIn("messages", payload)
        self.assertNotIn("instructions", payload)
        self.assertEqual(timeout, 150)

    def test_timeout_defaults_when_unset(self):
        transport = self.FakeTransport(
            {
                "output": [
                    {
                        "type": "message",
                        "role": "assistant",
                        "content": [{"type": "output_text", "text": "ok"}],
                    }
                ]
            }
        )
        service = app_core.ChatService(transport)
        config = self._config()
        del config["request_timeout"]

        service.chat(config, "hi")

        self.assertEqual(transport.calls[0][3], 120)

    def test_session_key_is_sanitized_and_capped(self):
        self.assertEqual(app_core.sanitize_session_key("own\ner\t"), "owner")
        self.assertEqual(len(app_core.sanitize_session_key("k" * 300)), 256)

    def test_extract_output_text_skips_tool_steps_and_joins_text(self):
        text = app_core.extract_output_text(
            {
                "output": [
                    {"type": "function_call", "name": "x"},
                    {"type": "function_call_output", "output": "tool result"},
                    {
                        "type": "message",
                        "role": "assistant",
                        "content": [
                            {"type": "output_text", "text": "答案A。"},
                            {"type": "output_text", "text": "答案B。"},
                        ],
                    },
                ]
            }
        )
        self.assertEqual(text, "答案A。答案B。")

    def test_extract_output_text_raises_when_no_assistant_text(self):
        with self.assertRaises(RuntimeError):
            app_core.extract_output_text({"output": [{"type": "function_call"}]})


if __name__ == "__main__":
    unittest.main()
