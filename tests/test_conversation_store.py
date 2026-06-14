import tempfile
import unittest
from pathlib import Path

from llm_admin.conversation import ConversationStore


class ConversationStoreTest(unittest.TestCase):
    def test_new_conversation_creates_isolated_id_for_device(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = ConversationStore(Path(tmp) / "conversations.json")

            first = store.new_conversation("device-a")
            second = store.new_conversation("device-a")

        self.assertNotEqual(first, second)
        self.assertTrue(first)
        self.assertTrue(second)

    def test_append_and_read_history_with_limit(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = ConversationStore(Path(tmp) / "conversations.json")
            conversation_id = store.new_conversation("device-a")
            store.append_turn("device-a", conversation_id, "q1", "a1")
            store.append_turn("device-a", conversation_id, "q2", "a2")
            store.append_turn("device-a", conversation_id, "q3", "a3")

            history = store.history("device-a", conversation_id, limit=2)

        self.assertEqual(
            history,
            [
                {"role": "user", "content": "q2"},
                {"role": "assistant", "content": "a2"},
                {"role": "user", "content": "q3"},
                {"role": "assistant", "content": "a3"},
            ],
        )

    def test_history_is_isolated_by_device_and_conversation(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = ConversationStore(Path(tmp) / "conversations.json")
            a1 = store.new_conversation("device-a")
            a2 = store.new_conversation("device-a")
            b1 = store.new_conversation("device-b")
            store.append_turn("device-a", a1, "a1q", "a1a")
            store.append_turn("device-a", a2, "a2q", "a2a")
            store.append_turn("device-b", b1, "b1q", "b1a")

            self.assertEqual(store.history("device-a", a1, limit=8)[0]["content"], "a1q")
            self.assertEqual(store.history("device-a", a2, limit=8)[0]["content"], "a2q")
            self.assertEqual(store.history("device-b", b1, limit=8)[0]["content"], "b1q")


if __name__ == "__main__":
    unittest.main()
