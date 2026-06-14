import unittest
from unittest import mock

from device_gateway import device_simulator


class DeviceSimulatorTest(unittest.TestCase):
    def test_chat_request_includes_device_token_and_conversation(self):
        calls = []

        def fake_post_json(url, headers, payload):
            calls.append((url, headers, payload))
            return {
                "device_id": "esp32-dev-001",
                "conversation_id": "conv-1",
                "answer": "你好",
            }

        client = device_simulator.DeviceClient(
            server="http://127.0.0.1:8766",
            device_id="esp32-dev-001",
            token="device-secret",
            post_json=fake_post_json,
        )

        response = client.chat("你好")

        self.assertEqual(response["answer"], "你好")
        self.assertEqual(client.conversation_id, "conv-1")
        url, headers, payload = calls[0]
        self.assertEqual(url, "http://127.0.0.1:8766/api/chat")
        self.assertEqual(headers["X-Device-Token"], "device-secret")
        self.assertEqual(payload["device_id"], "esp32-dev-001")
        self.assertEqual(payload["prompt"], "你好")

    def test_new_conversation_updates_local_conversation_id(self):
        def fake_post_json(url, headers, payload):
            return {"device_id": "esp32-dev-001", "conversation_id": "conv-new"}

        client = device_simulator.DeviceClient(
            server="http://127.0.0.1:8766",
            device_id="esp32-dev-001",
            token="device-secret",
            post_json=fake_post_json,
        )

        conversation_id = client.new_conversation()

        self.assertEqual(conversation_id, "conv-new")
        self.assertEqual(client.conversation_id, "conv-new")


if __name__ == "__main__":
    unittest.main()
