import json
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List


Message = Dict[str, str]


class ConversationStore:
    def __init__(self, path: Path):
        self.path = path

    def new_conversation(self, device_id: str) -> str:
        conversation_id = uuid.uuid4().hex
        data = self._read()
        now = self._now()
        data.setdefault(device_id, {})[conversation_id] = {
            "messages": [],
            "created_at": now,
            "updated_at": now,
        }
        self._write(data)
        return conversation_id

    def append_turn(
        self,
        device_id: str,
        conversation_id: str,
        prompt: str,
        answer: str,
    ) -> None:
        data = self._read()
        now = self._now()
        conversation = data.setdefault(device_id, {}).setdefault(
            conversation_id,
            {
                "messages": [],
                "created_at": now,
                "updated_at": now,
            },
        )
        conversation["messages"].append({"role": "user", "content": prompt})
        conversation["messages"].append({"role": "assistant", "content": answer})
        conversation["updated_at"] = now
        self._write(data)

    def history(self, device_id: str, conversation_id: str, limit: int) -> List[Message]:
        conversation = self._read().get(device_id, {}).get(conversation_id, {})
        messages = conversation.get("messages", [])
        if limit <= 0:
            return []
        return messages[-limit * 2 :]

    def _read(self) -> Dict:
        if not self.path.exists():
            return {}
        return json.loads(self.path.read_text(encoding="utf-8"))

    def _write(self, data: Dict) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text(
            json.dumps(data, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )

    @staticmethod
    def _now() -> str:
        return datetime.now(timezone.utc).isoformat()
