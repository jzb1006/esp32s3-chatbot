import argparse
import json
import urllib.request
from typing import Callable, Dict, Optional


def post_json(url: str, headers: Dict[str, str], payload: Dict) -> Dict:
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=body,
        headers={
            **headers,
            "Content-Type": "application/json",
        },
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.loads(response.read().decode("utf-8"))


class DeviceClient:
    def __init__(
        self,
        server: str,
        device_id: str,
        token: str,
        post_json: Callable[[str, Dict[str, str], Dict], Dict] = post_json,
    ):
        self.server = server.rstrip("/")
        self.device_id = device_id
        self.token = token
        self.conversation_id: Optional[str] = None
        self._post_json = post_json

    def chat(self, prompt: str) -> Dict:
        payload = {
            "device_id": self.device_id,
            "prompt": prompt,
        }
        if self.conversation_id:
            payload["conversation_id"] = self.conversation_id
        response = self._post_json(
            f"{self.server}/api/chat",
            {"X-Device-Token": self.token},
            payload,
        )
        self.conversation_id = response.get("conversation_id", self.conversation_id)
        return response

    def new_conversation(self) -> str:
        response = self._post_json(
            f"{self.server}/api/conversations/new",
            {"X-Device-Token": self.token},
            {"device_id": self.device_id},
        )
        self.conversation_id = response["conversation_id"]
        return self.conversation_id


def main() -> None:
    parser = argparse.ArgumentParser(description="ESP32-S3 LLM device simulator")
    parser.add_argument("--server", required=True)
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--token", default="")
    args = parser.parse_args()

    client = DeviceClient(args.server, args.device_id, args.token)
    print("Type /new to start a new conversation, /exit to quit.")
    while True:
        line = input("> ").strip()
        if not line:
            continue
        if line == "/exit":
            break
        if line == "/new":
            conversation_id = client.new_conversation()
            print(f"conversation_id={conversation_id}")
            continue
        response = client.chat(line)
        print(response.get("answer", response))


if __name__ == "__main__":
    main()
