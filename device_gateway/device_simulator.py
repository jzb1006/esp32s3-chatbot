import argparse
import json
import urllib.request
from typing import Callable, Dict, Iterable, Optional


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


def post_stream(url: str, headers: Dict[str, str], payload: Dict) -> Iterable[Dict]:
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
    with urllib.request.urlopen(request, timeout=120) as response:
        event_name = "message"
        data_lines = []
        for raw_line in response:
            line = raw_line.decode("utf-8").rstrip("\r\n")
            if not line:
                if data_lines:
                    data = "\n".join(data_lines)
                    try:
                        parsed = json.loads(data)
                    except json.JSONDecodeError:
                        parsed = data
                    yield {"event": event_name, "data": parsed}
                event_name = "message"
                data_lines = []
                continue
            if line.startswith("event:"):
                event_name = line[len("event:"):].strip()
            elif line.startswith("data:"):
                data_lines.append(line[len("data:"):].strip())


class DeviceClient:
    def __init__(
        self,
        server: str,
        device_id: str,
        token: str,
        post_json: Callable[[str, Dict[str, str], Dict], Dict] = post_json,
        post_stream: Callable[[str, Dict[str, str], Dict], Iterable[Dict]] = post_stream,
    ):
        self.server = server.rstrip("/")
        self.device_id = device_id
        self.token = token
        self.conversation_id: Optional[str] = None
        self._post_json = post_json
        self._post_stream = post_stream

    def _chat_payload(self, prompt: str) -> Dict:
        payload = {
            "device_id": self.device_id,
            "prompt": prompt,
        }
        if self.conversation_id:
            payload["conversation_id"] = self.conversation_id
        return payload

    def chat(self, prompt: str) -> Dict:
        payload = self._chat_payload(prompt)
        response = self._post_json(
            f"{self.server}/api/chat",
            {"X-Device-Token": self.token},
            payload,
        )
        self.conversation_id = response.get("conversation_id", self.conversation_id)
        return response

    def stream_chat(self, prompt: str) -> Iterable[Dict]:
        payload = self._chat_payload(prompt)
        for event in self._post_stream(
            f"{self.server}/api/chat/stream",
            {"X-Device-Token": self.token},
            payload,
        ):
            data = event.get("data")
            if isinstance(data, dict) and data.get("conversation_id"):
                self.conversation_id = data["conversation_id"]
            yield event

    def new_conversation(self) -> str:
        response = self._post_json(
            f"{self.server}/api/conversations/new",
            {"X-Device-Token": self.token},
            {"device_id": self.device_id},
        )
        self.conversation_id = response["conversation_id"]
        return self.conversation_id


def main() -> None:
    parser = argparse.ArgumentParser(description="ESP32-S3 device simulator")
    parser.add_argument("--server", required=True)
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--token", default="")
    parser.add_argument("--stream", action="store_true")
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
        if args.stream:
            for event in client.stream_chat(line):
                data = event.get("data")
                if isinstance(data, dict) and "delta" in data:
                    print(data["delta"], end="", flush=True)
                elif event.get("event") == "error":
                    print(data)
            print()
        else:
            response = client.chat(line)
            print(response.get("answer", response))


if __name__ == "__main__":
    main()
